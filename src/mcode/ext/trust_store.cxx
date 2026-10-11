#include "mcode/ext/trust_store.hxx"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "mcode/platform/seams.hxx"

namespace mcode::ext {

	namespace {

		constexpr std::string_view FORMAT_VERSION = "v1";
		constexpr std::string_view DENY_HASH = "-";
		constexpr std::size_t FIELD_COUNT = 7;

		auto split_fields( const std::string_view line ) -> std::vector< std::string_view > {
			auto fields = std::vector< std::string_view >{ };
			auto start = std::size_t{ 0 };

			while ( true ) {
				const auto tab = line.find( '\t', start );

				if ( tab == std::string_view::npos ) {
					fields.push_back( line.substr( start ) );

					break;
				}

				fields.push_back( line.substr( start, tab - start ) );
				start = tab + 1;
			}

			return fields;
		}

		auto parse_scope( const std::string_view text, trust_scope& out ) -> bool {
			if ( text == "user" ) {
				out = trust_scope::user;

				return true;
			}

			if ( text == "project" ) {
				out = trust_scope::project;

				return true;
			}

			return false;
		}

		auto parse_int( const std::string_view text, std::int64_t& out ) -> bool {
			if ( text.empty( ) ) {
				return false;
			}

			auto value = std::int64_t{ 0 };

			for ( const auto character : text ) {
				if ( character < '0' || character > '9' ) {
					return false;
				}

				value = value * 10 + ( character - '0' );
			}

			out = value;

			return true;
		}

	}

	auto to_string( const trust_scope scope ) noexcept -> std::string_view {
		switch ( scope ) {
			case trust_scope::user: return "user";
			case trust_scope::project: return "project";
		}

		return "user";
	}

	auto trust_store::default_path( ) -> result< std::filesystem::path > {
		// Machine state, not configuration: it must sit outside every workspace
		// root so the permission engine's workspace-write rules cannot reach it.
		auto base = platform::app_data_path( platform::data_kind::state );

		if ( !base ) {
			return std::unexpected( base.error( ) );
		}

		return *base / "trust";
	}

	auto trust_store::load( const std::filesystem::path& path ) -> result< trust_store > {
		auto store = trust_store{ };
		store.path_ = path;

		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			// Absent is the normal first-run state, not an error.
			return store;
		}

		auto line = std::string{ };
		auto number = std::size_t{ 0 };

		while ( std::getline( stream, line ) ) {
			++number;

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.pop_back( );
			}

			if ( line.empty( ) ) {
				continue;
			}

			const auto fields = split_fields( line );

			if ( fields.size( ) != FIELD_COUNT || fields[ 0 ] != FORMAT_VERSION ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " is not a valid v1 record" ) );
			}

			const auto is_allow = fields[ 1 ] == "allow";
			const auto is_deny = fields[ 1 ] == "deny";

			if ( !is_allow && !is_deny ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has an unknown decision" ) );
			}

			auto record = trust_record{ };

			if ( !parse_scope( fields[ 2 ], record.scope ) ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has an unknown scope" ) );
			}

			record.name = std::string{ fields[ 3 ] };

			if ( record.name.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has an empty name" ) );
			}

			if ( !parse_int( fields[ 4 ], record.api_version ) ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has an unparsable api_version" ) );
			}

			record.tree_sha256 = is_deny ? std::string{ DENY_HASH } : std::string{ fields[ 5 ] };

			if ( is_allow && record.tree_sha256.size( ) != 64 ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has a malformed digest" ) );
			}

			record.root = std::filesystem::path{ std::string{ fields[ 6 ] } };

			if ( record.root.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"trust store " + path.string( ) + " line " + std::to_string( number )
						+ " has an empty root" ) );
			}

			store.records_.push_back( std::move( record ) );
		}

		if ( stream.bad( ) ) {
			return std::unexpected( fail( errc::io, "cannot read " + path.string( ) ) );
		}

		return store;
	}

	auto trust_store::record_for( const trust_scope scope, const std::string_view name ) const
		-> const trust_record* {
		for ( const auto& record : records_ ) {
			if ( record.scope == scope && record.name == name ) {
				return &record;
			}
		}

		return nullptr;
	}

	auto trust_store::decide( const trust_scope scope, const std::string_view name,
		const std::int64_t api_version ) const -> trust_decision {
		const auto* record = record_for( scope, name );

		if ( record == nullptr ) {
			return trust_decision::ungranted;
		}

		if ( record->tree_sha256 == DENY_HASH ) {
			return trust_decision::denied;
		}

		// An api_version change invalidates the grant: the surface moved under it.
		if ( record->api_version != api_version ) {
			return trust_decision::ungranted;
		}

		return trust_decision::granted;
	}

	auto trust_store::write_to( const std::filesystem::path& path,
		const std::vector< trust_record >& records ) -> status {
		auto error = std::error_code{ };
		std::filesystem::create_directories( path.parent_path( ), error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot create " + path.parent_path( ).string( ) + ": " + error.message( ) ) );
		}

		auto text = std::ostringstream{ };

		for ( const auto& record : records ) {
			text << FORMAT_VERSION << '\t'
				<< ( record.tree_sha256 == DENY_HASH ? "deny" : "allow" ) << '\t'
				<< to_string( record.scope ) << '\t'
				<< record.name << '\t'
				<< record.api_version << '\t'
				<< record.tree_sha256 << '\t'
				<< record.root.generic_string( ) << '\n';
		}

		auto temporary = path;
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

		std::filesystem::rename( temporary, path, error );

		if ( error ) {
			std::filesystem::remove( temporary, error );

			return std::unexpected( fail( errc::io,
				"cannot replace " + path.string( ) + ": " + error.message( ) ) );
		}

		return { };
	}

	auto trust_store::save( ) const -> status {
		return write_to( path_, records_ );
	}

	auto trust_store::grant( const trust_record& record ) -> status {
		auto next = records_;

		next.erase( std::remove_if( next.begin( ), next.end( ),
			[ &record ]( const trust_record& existing ) {
				return existing.scope == record.scope && existing.name == record.name;
			} ), next.end( ) );

		next.push_back( record );

		const auto saved = write_to( path_, next );

		if ( !saved ) {
			return saved;
		}

		records_ = std::move( next );

		return { };
	}

	auto trust_store::revoke( const trust_scope scope, const std::string_view name ) -> status {
		auto next = records_;

		next.erase( std::remove_if( next.begin( ), next.end( ),
			[ scope, name ]( const trust_record& existing ) {
				return existing.scope == scope && existing.name == name;
			} ), next.end( ) );

		const auto saved = write_to( path_, next );

		if ( !saved ) {
			return saved;
		}

		records_ = std::move( next );

		return { };
	}

}
