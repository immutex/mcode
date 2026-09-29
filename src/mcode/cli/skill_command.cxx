#include "mcode/cli/skill_command.hxx"

#include <cstdio>
#include <map>
#include <filesystem>

#include "mcode/cli/exec.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/skills/discovery.hxx"
#include "mcode/skills/index.hxx"

namespace mcode::cli {

	namespace {

		auto user_skills_root( ) -> std::filesystem::path {
			auto root = std::filesystem::path{ };

			if ( const auto base = platform::app_data_path( platform::data_kind::config ) ) {
				root = *base / "skills";
			}

			return root;
		}

		auto build_options( const std::filesystem::path& workspace ) -> skills::discovery_options {
			auto options = skills::discovery_options{ };
			options.workspace = workspace;
			options.user_root = user_skills_root( );

			if ( const auto bundled = platform::executable_directory( ) ) {
				options.extension_roots.push_back( *bundled / "extensions" );
			}

			return options;
		}

		auto command_list( const std::filesystem::path& workspace ) -> int {
			const auto report = skills::discover_skills( build_options( workspace ) );

			if ( report.entries.empty( ) && report.rejected.empty( ) ) {
				std::printf( "no skills discovered\n" );

				return to_int( exit_code::success );
			}

			// Through `index_lines`, the same rows the prompt is built from, so
			// what the user sees here is what the model sees. The origin comes
			// from the report: the index line does not carry it, because the
			// prompt has no use for it.
			auto origins = std::map< std::string, std::string >{ };

			for ( const auto& entry : report.entries ) {
				origins.insert_or_assign( entry.name,
					std::string{ skills::to_string( entry.origin ) } );
			}

			for ( const auto& line : skills::index_lines( report.entries ) ) {
				const auto found = origins.find( line.name );
				const auto origin = found != origins.end( ) ? found->second : std::string{ };

				// A hidden skill is listed, not omitted: `disable-model-invocation`
				// keeps it out of the prompt, not out of the repository. Marking it
				// is how a user sees that the skill exists and why the model has
				// not heard of it.
				std::printf( "%-24s %-9s %s%s\n", line.name.c_str( ),
					origin.c_str( ), line.description.c_str( ),
					line.hidden ? "  [hidden from the model]" : "" );
			}

			for ( const auto& reason : report.rejected ) {
				std::printf( "rejected: %s\n", reason.c_str( ) );
			}

			return to_int( exit_code::success );
		}

		auto command_validate( const std::filesystem::path& workspace ) -> int {
			const auto report = skills::discover_skills( build_options( workspace ) );

			for ( const auto& entry : report.entries ) {
				const auto origin = std::string{ skills::to_string( entry.origin ) };

				std::printf( "ok       %s (%s)\n", entry.name.c_str( ),
					origin.c_str( ) );
			}

			for ( const auto& entry : report.entries ) {
				if ( !skills::hidden_from_model( entry ) ) {
					continue;
				}

				std::printf( "hidden   %s (disable-model-invocation)\n",
					entry.name.c_str( ) );
			}

			for ( const auto& reason : report.rejected ) {
				std::printf( "invalid  %s\n", reason.c_str( ) );
			}

			return report.rejected.empty( ) ? to_int( exit_code::success )
				: to_int( exit_code::verification_failed );
		}

	} // namespace

	auto run_skill( const std::vector< std::string >& arguments ) -> int {
		if ( arguments.empty( ) ) {
			std::fprintf( stderr,
				"usage: mcode skill list|validate [--cwd <path>]\n" );

			return to_int( exit_code::usage_error );
		}

		auto command = std::string{ arguments.front( ) };

		if ( command != "list" && command != "validate" ) {
			std::fprintf( stderr, "mcode: unknown skill command '%s'\n",
				command.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		auto workspace = std::filesystem::current_path( );

		for ( auto index = std::size_t{ 1 }; index < arguments.size( ); ++index ) {
			if ( arguments[ index ] == "--cwd" && index + 1 < arguments.size( ) ) {
				workspace = arguments[ ++index ];

				continue;
			}

			const auto argument = std::string{ arguments[ index ] };

			std::fprintf( stderr, "mcode: unknown skill argument '%s'\n",
				argument.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		return command == "list" ? command_list( workspace ) : command_validate( workspace );
	}

}
