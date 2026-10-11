#include "mcode/ext/lockfile.hxx"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "mcode/platform/seams.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::ext {

	namespace {

		constexpr std::string_view GIT_PREFIX = "git:";
		constexpr std::string_view HOST = "github.com";
		constexpr std::string_view ENTRY_PREFIX = "extension.";

		// `owner/repo`, one slash, no scheme and no traversal: the coordinate is
		// interpolated into a git URL, so anything else is a redirect primitive.
		auto valid_slug( const std::string_view slug ) -> bool {
			const auto slash = slug.find( '/' );

			if ( slash == std::string_view::npos || slash == 0 || slash + 1 >= slug.size( ) ) {
				return false;
			}

			if ( slug.find( '/', slash + 1 ) != std::string_view::npos ) {
				return false;
			}

			for ( const auto character : slug ) {
				const auto allowed = ( character >= 'A' && character <= 'Z' )
					|| ( character >= 'a' && character <= 'z' )
					|| ( character >= '0' && character <= '9' ) || character == '.' ||
					character == '-' || character == '_' || character == '/';

				if ( !allowed ) {
					return false;
				}
			}

			return slug.find( ".." ) == std::string_view::npos;
		}

		auto join_permissions( const std::vector< std::string >& permissions ) -> std::string {
			auto out = std::string{ };

			for ( const auto& permission : permissions ) {
				if ( !out.empty( ) ) {
					out += ", ";
				}

				out += '"' + permission + '"';
			}

			return out;
		}

		auto split_permissions( const std::string& text ) -> std::vector< std::string > {
			auto out = std::vector< std::string >{ };
			auto current = std::string{ };
			auto quoted = false;

			for ( const auto character : text ) {
				if ( character == '"' ) {
					quoted = !quoted;

					continue;
				}

				if ( character == ',' && !quoted ) {
					if ( !current.empty( ) ) {
						out.push_back( current );
						current.clear( );
					}

					continue;
				}

				if ( character != ' ' ) {
					current += character;
				}
			}

			if ( !current.empty( ) ) {
				out.push_back( current );
			}

			return out;
		}

	}

	auto parse_git_coordinate( const std::string_view coordinate, const std::string_view ref,
		std::string& url_out, std::string& ref_out ) -> status {
		if ( !coordinate.starts_with( GIT_PREFIX ) ) {
			return std::unexpected( fail( errc::config,
				"expected an install coordinate like git:owner/repo@v1, got '"
					+ std::string{ coordinate } + "'" ) );
		}

		const auto slug = coordinate.substr( GIT_PREFIX.size( ) );

		if ( !valid_slug( slug ) ) {
			return std::unexpected( fail( errc::config,
				"install coordinate '" + std::string{ slug }
					+ "' is not owner/repo; a full URL is refused so a lockfile cannot "
					  "be pointed at an arbitrary endpoint" ) );
		}

		if ( ref.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"an install needs a ref: git:" + std::string{ slug } + "@<tag|branch|sha>" ) );
		}

		url_out = "https://" + std::string{ HOST } + "/" + std::string{ slug } + ".git";
		ref_out = std::string{ ref };

		return { };
	}

	auto lockfile::project_path( const std::filesystem::path& workspace )
		-> std::filesystem::path {
		return workspace / ".mcode" / "extensions.lock";
	}

	auto lockfile::user_path( ) -> result< std::filesystem::path > {
		auto base = platform::app_data_path( platform::data_kind::state );

		if ( !base ) {
			return std::unexpected( base.error( ) );
		}

		return *base / "extensions.lock";
	}

	auto lockfile::load( const std::filesystem::path& path ) -> result< lockfile > {
		auto out = lockfile{ };
		out.path_ = path;

		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return out;
		}

		auto buffer = std::ostringstream{ };
		buffer << stream.rdbuf( );

		auto parsed = toml::parse( buffer.str( ) );

		if ( !parsed ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": " + parsed.error( ).msg ) );
		}

		auto names = std::vector< std::string >{ };

		for ( const auto& [key, unused_value] : parsed->keys( ) ) {
			if ( !key.starts_with( ENTRY_PREFIX ) ) {
				return std::unexpected( fail( errc::config,
					path.string( ) + ": unknown key '" + key + "'" ) );
			}

			const auto name = key.substr( ENTRY_PREFIX.size( ) );

			if ( name.empty( ) || name.find( '.' ) != std::string::npos ) {
				return std::unexpected( fail( errc::config,
					path.string( ) + ": malformed entry key '" + key + "'" ) );
			}

			names.push_back( name );
		}

		for ( const auto& name : names ) {
			auto entry = lock_entry{ };
			entry.name = name;

			auto get = [ & ]( const std::string_view field ) -> std::string {
				return parsed->get_string( std::string{ ENTRY_PREFIX } + name + "." + std::string{ field } )
					.value_or( std::string{ } );
			};

			entry.source = get( "source" );
			entry.ref = get( "ref" );
			entry.commit = get( "commit" );
			entry.commit_format = get( "commit_format" );
			entry.tree_sha256 = get( "tree_sha256" );
			entry.api_version = parsed->get_int(
				std::string{ ENTRY_PREFIX } + name + ".api_version" ).value_or( 0 );
			entry.permissions = split_permissions( get( "permissions" ) );

			out.entries_.push_back( std::move( entry ) );
		}

		return out;
	}

	auto lockfile::find( const std::string_view name ) const -> const lock_entry* {
		for ( const auto& entry : entries_ ) {
			if ( entry.name == name ) {
				return &entry;
			}
		}

		return nullptr;
	}

	auto lockfile::put( const lock_entry& entry ) -> void {
		for ( auto& existing : entries_ ) {
			if ( existing.name == entry.name ) {
				existing = entry;

				return;
			}
		}

		entries_.push_back( entry );
	}

	auto lockfile::save( ) const -> status {
		auto error = std::error_code{ };
		std::filesystem::create_directories( path_.parent_path( ), error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot create " + path_.parent_path( ).string( ) + ": " + error.message( ) ) );
		}

		auto sorted = entries_;

		std::sort( sorted.begin( ), sorted.end( ),
			[]( const lock_entry& left, const lock_entry& right ) {
				return left.name < right.name;
			} );

		auto text = std::ostringstream{ };

		for ( const auto& entry : sorted ) {
			text << "[extension." << entry.name << "]\n"
				<< "source = \"" << entry.source << "\"\n"
				<< "ref = \"" << entry.ref << "\"\n"
				<< "commit = \"" << entry.commit << "\"\n"
				<< "commit_format = \"" << entry.commit_format << "\"\n"
				<< "tree_sha256 = \"" << entry.tree_sha256 << "\"\n"
				<< "api_version = " << entry.api_version << "\n"
				<< "permissions = [" << join_permissions( entry.permissions ) << "]\n\n";
		}

		auto temporary = path_;
		temporary += ".tmp";

		{
			auto stream = std::ofstream{ temporary, std::ios::binary | std::ios::trunc };

			if ( !stream ) {
				return std::unexpected( fail( errc::io, "cannot write " + temporary.string( ) ) );
			}

			stream << text.str( );

			if ( !stream ) {
				return std::unexpected( fail( errc::io, "cannot write " + temporary.string( ) ) );
			}
		}

		std::filesystem::rename( temporary, path_, error );

		if ( error ) {
			std::filesystem::remove( temporary, error );

			return std::unexpected( fail( errc::io,
				"cannot replace " + path_.string( ) + ": " + error.message( ) ) );
		}

		return { };
	}

}
