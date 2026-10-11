#include "mcode/cli/ext_command.hxx"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include "mcode/cli/exec.hxx"
#include "mcode/ext/integrity.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/ext/lockfile.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/ext/trust_store.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/text.hxx"

namespace mcode::cli {

	namespace {

		constexpr std::string_view EXTENSIONS_SUBDIRECTORY = "extensions";

		struct discovered {
			std::string name;
			std::filesystem::path directory;
			ext::manifest details;
			ext::trust_scope scope = ext::trust_scope::user;
			std::string tree_sha256;
			std::string problem;
		};

		auto user_extension_root( ) -> std::filesystem::path {
			if ( const auto base = platform::app_data_path( platform::data_kind::config ) ) {
				return *base / EXTENSIONS_SUBDIRECTORY;
			}

			return { };
		}

		auto bundled_extension_root( ) -> std::filesystem::path {
			if ( const auto base = platform::executable_directory( ) ) {
				return *base / EXTENSIONS_SUBDIRECTORY;
			}

			return { };
		}

		// The same roots the loader walks, so `ext list` cannot disagree with what
		// a session would load. The bundled root is last: it is the baseline.
		auto roots_for( const std::filesystem::path& workspace ) -> std::vector< std::filesystem::path > {
			auto roots = std::vector< std::filesystem::path >{ };
			const auto project = ext::project_extension_root( workspace );

			roots.push_back( project );
			roots.push_back( user_extension_root( ) );
			roots.push_back( bundled_extension_root( ) );

			return roots;
		}

		auto scope_for( const std::filesystem::path& workspace,
			const std::filesystem::path& directory ) -> ext::trust_scope {
			return path_is_within( ext::project_extension_root( workspace ), directory )
				? ext::trust_scope::project
				: ext::trust_scope::user;
		}

		auto discover( const std::filesystem::path& workspace ) -> std::vector< discovered > {
			auto out = std::vector< discovered >{ };

			for ( const auto& root : roots_for( workspace ) ) {
				auto error = std::error_code{ };

				if ( root.empty( ) || !std::filesystem::exists( root, error ) ) {
					continue;
				}

				for ( const auto& entry : std::filesystem::directory_iterator{ root, error } ) {
					auto status_error = std::error_code{ };
					const auto kind = entry.symlink_status( status_error );

					if ( status_error || !std::filesystem::is_directory( kind ) ) {
						continue;
					}

					if ( !std::filesystem::exists( entry.path( ) / "ext.toml" ) ) {
						continue;
					}

					auto found = discovered{ };
					found.directory = entry.path( );
					found.scope = scope_for( workspace, entry.path( ) );

					auto details = ext::load_manifest( entry.path( ) );

					if ( !details ) {
						found.name = entry.path( ).filename( ).string( );
						found.problem = details.error( ).msg;
						out.push_back( std::move( found ) );

						continue;
					}

					found.name = details->name;
					found.details = *details;

					auto snapshot = ext::snapshot_extension( entry.path( ) );

					if ( !snapshot ) {
						found.problem = snapshot.error( ).msg;
					} else {
						found.tree_sha256 = snapshot->tree_sha256;
					}

					out.push_back( std::move( found ) );
				}
			}

			std::sort( out.begin( ), out.end( ),
				[]( const discovered& left, const discovered& right ) {
					return left.name < right.name;
				} );

			return out;
		}

		auto trust_store_for( ) -> ext::trust_store {
			const auto path = ext::trust_store::default_path( );

			if ( !path ) {
				return ext::trust_store{ };
			}

			auto store = ext::trust_store::load( *path );

			if ( !store ) {
				std::fprintf( stderr, "mcode: %s\n", store.error( ).msg.c_str( ) );

				return ext::trust_store{ };
			}

			return std::move( *store );
		}

		auto trust_label( const ext::trust_store& store, const discovered& found )
			-> std::string {
			if ( found.scope != ext::trust_scope::project ) {
				return "trusted-by-location";
			}

			const auto decision = store.decide( found.scope, found.name,
				found.details.api_version );

			if ( decision == ext::trust_decision::denied ) {
				return "denied";
			}

			if ( decision == ext::trust_decision::ungranted ) {
				return "ungranted";
			}

			const auto* record = store.record_for( found.scope, found.name );

			if ( record == nullptr || record->tree_sha256 != found.tree_sha256 ) {
				return "changed";
			}

			return "granted";
		}

		auto command_list( const std::filesystem::path& workspace ) -> int {
			const auto found = discover( workspace );
			const auto store = trust_store_for( );

			if ( found.empty( ) ) {
				std::printf( "no extensions discovered\n" );

				return to_int( exit_code::success );
			}

			for ( const auto& entry : found ) {
				std::printf( "%-22s %-9s %-18s %s\n", entry.name.c_str( ),
					ext::to_string( entry.scope ).data( ),
					entry.problem.empty( ) ? trust_label( store, entry ).c_str( ) : "invalid",
					entry.problem.empty( ) ? entry.details.version.c_str( )
						: entry.problem.c_str( ) );
			}

			return to_int( exit_code::success );
		}

		auto command_trust( const std::filesystem::path& workspace, const std::string& name ) -> int {
			const auto found = discover( workspace );

			const auto match = std::find_if( found.begin( ), found.end( ),
				[ &name ]( const discovered& entry ) { return entry.name == name; } );

			if ( match == found.end( ) ) {
				std::fprintf( stderr, "mcode: no discovered extension named '%s'\n", name.c_str( ) );

				return to_int( exit_code::usage_error );
			}

			if ( !match->problem.empty( ) ) {
				std::fprintf( stderr, "mcode: %s is invalid: %s\n", name.c_str( ),
					match->problem.c_str( ) );

				return to_int( exit_code::usage_error );
			}

			if ( match->scope != ext::trust_scope::project ) {
				std::fprintf( stderr,
					"mcode: '%s' is trusted by location and needs no grant\n", name.c_str( ) );

				return to_int( exit_code::success );
			}

			auto store = trust_store_for( );

			auto record = ext::trust_record{ };
			record.scope = ext::trust_scope::project;
			record.name = match->name;
			record.api_version = match->details.api_version;
			record.tree_sha256 = match->tree_sha256;
			record.root = match->directory;

			if ( const auto granted = store.grant( record ); !granted ) {
				std::fprintf( stderr, "mcode: %s\n", granted.error( ).msg.c_str( ) );

				return to_int( exit_code::usage_error );
			}

			std::printf( "granted %s (%s)\n", match->name.c_str( ), match->tree_sha256.c_str( ) );

			if ( !match->details.permissions.empty( ) ) {
				std::printf( "  permissions: " );

				for ( const auto& permission : match->details.permissions ) {
					std::printf( "%s ", permission.c_str( ) );
				}

				std::printf( "\n" );
			}

			const auto suspicious = text::count_invisible( match->directory );

			if ( suspicious > 0 ) {
				std::printf( "  warning: %zu invisible or confusable codepoints in the extension "
					"source; read it before trusting it\n", suspicious );
			}

			return to_int( exit_code::success );
		}

		auto command_deny( const std::string& name ) -> int {
			auto store = trust_store_for( );

			auto record = ext::trust_record{ };
			record.scope = ext::trust_scope::project;
			record.name = name;
			record.api_version = 0;
			record.tree_sha256 = "-";

			if ( const auto denied = store.grant( record ); !denied ) {
				std::fprintf( stderr, "mcode: %s\n", denied.error( ).msg.c_str( ) );

				return to_int( exit_code::usage_error );
			}

			std::printf( "denied %s\n", name.c_str( ) );

			return to_int( exit_code::success );
		}

	}

	auto run_ext( const std::vector< std::string >& arguments ) -> int {
		if ( arguments.empty( ) ) {
			std::fprintf( stderr,
				"usage: mcode ext list|trust|deny <name>|install git:owner/repo@<ref>\n" );

			return to_int( exit_code::usage_error );
		}

		const auto& command = arguments.front( );
		auto workspace = std::filesystem::current_path( );

		if ( command == "list" ) {
			return command_list( workspace );
		}

		if ( command == "trust" || command == "deny" ) {
			if ( arguments.size( ) < 2 ) {
				std::fprintf( stderr, "mcode: ext %s needs an extension name\n",
					command.c_str( ) );

				return to_int( exit_code::usage_error );
			}

			return command == "trust"
				? command_trust( workspace, arguments[ 1 ] )
				: command_deny( arguments[ 1 ] );
		}

		if ( command == "install" ) {
			if ( arguments.size( ) < 2 ) {
				std::fprintf( stderr, "mcode: ext install needs git:owner/repo@<ref>\n" );

				return to_int( exit_code::usage_error );
			}

			return run_ext_install( arguments[ 1 ] );
		}

		std::fprintf( stderr, "mcode: unknown ext command '%s'\n", command.c_str( ) );

		return to_int( exit_code::usage_error );
	}

}
