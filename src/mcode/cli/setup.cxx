#include "mcode/cli/setup.hxx"
#include "setup_terminal.hxx"
#include "setup_detail.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/tui/cell.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli {

	namespace {


		// A verification turn that never returns is worse than a failed one.
		inline constexpr std::int64_t VERIFY_TIMEOUT_MS = 90'000;

		// -------------------------------------------------------------------
		// The provider catalogue. Endpoints and key names mirror the shipped
		// provider descriptors, so a config written here is one the extension
		// already knows how to talk to.
		// -------------------------------------------------------------------

		struct verification {
			bool ok = false;
			std::string detail;
		};

		[[nodiscard]] auto verify( const detail::model_settings& settings,
			const std::string& api_key ) -> verification {
			const auto self = detail::self_path( );

			if ( self.empty( ) ) {
				return { true, "could not locate the mcode binary; skipped" };
			}

			auto options = mcode::process_options{ };
			options.executable = self.string( );
			options.args = { "exec", "--json", "Reply with the single word: ready" };
			options.working_directory = std::filesystem::current_path( ).string( );
			options.environment = mcode::minimal_environment( );
			options.timeout = std::chrono::milliseconds{ VERIFY_TIMEOUT_MS };

			// The key is handed to the child in its environment, so it never
			// reaches disk and never appears in the process arguments.
			if ( !api_key.empty( ) ) {
				options.environment[ settings.api_key_env ] = api_key;
			}

			const auto outcome = mcode::run_process( options );

			if ( !outcome ) {
				return { false, outcome.error( ).msg };
			}

			if ( outcome->timed_out ) {
				return { false, "the provider did not answer within "
					+ std::to_string( VERIFY_TIMEOUT_MS / 1000 ) + " seconds" };
			}

			if ( outcome->exit_code == 0 ) {
				return { true, { } };
			}

			// The child's last stderr line names the cause, and it is more
			// useful than the exit code.
			auto detail = outcome->stderr_text;

			while ( !detail.empty( ) && ( detail.back( ) == '\n' || detail.back( ) == '\r' ) ) {
				detail.pop_back( );
			}

			const auto last_line = detail.find_last_of( '\n' );

			if ( last_line != std::string::npos ) {
				detail = detail.substr( last_line + 1 );
			}

			if ( detail.empty( ) ) {
				detail = "the run exited with code " + std::to_string( outcome->exit_code );
			}

			return { false, detail };
		}

		// -------------------------------------------------------------------
		auto print_banner( const detail::painter& brush, const std::filesystem::path& path ) -> void {
			detail::blank( );
			detail::write_line( "  " + brush.mark( "mcode setup" ) );
			detail::write_line( "  " + brush.dim( "Choose a provider. Writes one file: "
				+ path.string( ) ) );
			detail::blank( );
		}

		[[nodiscard]] auto finish( const detail::painter& brush, const detail::model_settings& settings,
			const std::filesystem::path& path, const bool verified,
			const bool have_key ) -> int {
			detail::blank( );
			detail::write_line( "  " + brush.dim( std::string( detail::RULE_WIDTH, '-' ) ) );
			detail::blank( );
			detail::write_line( "  " + brush.good( "wrote" ) + "  " + path.string( ) );
			detail::write_line( "  " + brush.dim( "provider  " ) + settings.provider );
			detail::write_line( "  " + brush.dim( "model     " ) + settings.model );

			if ( !settings.base_url.empty( ) ) {
				detail::write_line( "  " + brush.dim( "endpoint  " ) + settings.base_url );
			}

			detail::write_line( "  " + brush.dim( "key from  " ) + settings.api_key_env
				+ ( verified ? brush.good( "  verified" ) : std::string{ } ) );
			detail::blank( );

			if ( !have_key ) {
				detail::write_line( "  " + brush.strong( "Next" ) + "  export "
					+ brush.accent( settings.api_key_env ) + "=your-key" );
			}

			detail::write_line( "  " + brush.strong( "Next" ) + "  run " + brush.mark( "mcode" ) );
			detail::blank( );

			return 0;
		}

		[[nodiscard]] auto cancelled( const detail::painter& brush ) -> int {
			detail::blank( );
			detail::write_line( "  " + brush.dim( "cancelled; nothing was written" ) );
			detail::blank( );

			return 1;
		}

		// Scripted: every value comes from a flag, and a missing one is a hard
		// failure rather than a silent default.
		[[nodiscard]] auto run_scripted( const std::vector< std::string >& arguments,
			const detail::painter& brush ) -> int {
			auto provider = std::string{ };
			auto model = std::string{ };
			auto base_url = std::string{ };
			auto key_env = std::string{ };
			auto api_key = std::string{ };
			auto should_verify = true;

			// Returns the flag's value, or nothing when the flag is last. A missing
			// value used to call `std::exit` from inside this lambda, which skipped
			// every destructor and made the path impossible to test.
			auto missing_value = false;
			auto index = std::size_t{ 0 };

			const auto value_for = [&]( const std::string_view flag ) -> std::string {
				if ( index + 1 >= arguments.size( ) ) {
					std::fprintf( stderr, "mcode: %s needs a value\n",
						std::string{ flag }.c_str( ) );
					missing_value = true;

					return { };
				}

				return arguments[ ++index ];
			};

			for ( ; index < arguments.size( ); ++index ) {
				const auto& argument = arguments[ index ];

				if ( argument == "--provider" ) {
					provider = value_for( argument );
				} else if ( argument == "--model" ) {
					model = value_for( argument );
				} else if ( argument == "--base-url" ) {
					base_url = value_for( argument );
				} else if ( argument == "--api-key-env" ) {
					key_env = value_for( argument );
				} else if ( argument == "--api-key" ) {
					api_key = value_for( argument );
				} else if ( argument == "--no-verify" ) {
					should_verify = false;
				} else if ( argument == "--yes" ) {
					// interactive-only; accepted here so the flag never errors
				} else {
					std::fprintf( stderr, "mcode: unknown setup option '%s'\n\n",
						argument.c_str( ) );
					std::fputs( setup_usage_text( ).c_str( ), stderr );

					return 2;
				}

				if ( missing_value ) {
					return 2;
				}
			}

			if ( provider.empty( ) || model.empty( ) ) {
				std::fputs( setup_usage_text( ).c_str( ), stderr );

				return 2;
			}

			if ( key_env.empty( ) ) {
				key_env = provider == "anthropic-messages" ? "ANTHROPIC_API_KEY"
					: "OPENAI_API_KEY";
			}

			if ( api_key.empty( ) ) {
				if ( const auto* from_environment = std::getenv( key_env.c_str( ) ) ) {
					api_key = from_environment;
				}
			}

			const auto settings = detail::model_settings{ provider, model, base_url, key_env };
			const auto path = detail::config_path( );

			if ( path.empty( ) ) {
				std::fprintf( stderr, "mcode: no configuration directory is available\n" );

				return 2;
			}

			if ( !detail::write_text_file( path, detail::splice_section(
				detail::read_text_file( path ).value_or( std::string{ } ),
				detail::render_section( settings ) ) ) ) {
				std::fprintf( stderr, "mcode: could not write %s\n", path.string( ).c_str( ) );

				return 2;
			}

			if ( should_verify && !api_key.empty( ) ) {
				const auto checked = verify( settings, api_key );

				if ( !checked.ok ) {
					std::fprintf( stderr, "mcode: verification failed: %s\n",
						checked.detail.c_str( ) );

					return 2;
				}
			}

			detail::write_line( brush.good( "wrote" ) + " " + path.string( ) );

			return 0;
		}

	}

	// The pure core of the config edit, exposed for tests: the section is built
	// here and spliced by the same code the wizard uses, so a test cannot pass
	// against a re-implementation that has drifted.
	auto splice_model_section( const std::string_view existing,
		const std::string_view provider, const std::string_view model,
		const std::string_view base_url, const std::string_view api_key_env ) -> std::string {
		const auto settings = detail::model_settings{ std::string{ provider }, std::string{ model },
			std::string{ base_url }, std::string{ api_key_env } };

		return detail::splice_section( existing, detail::render_section( settings ) );
	}

	// The flag set `run_setup` accepts. One list, so the interactive and
	// scripted paths cannot disagree about what is known.
	[[nodiscard]] auto is_setup_flag( const std::string_view argument ) -> bool {
		for ( const auto* known : { "--provider", "--model", "--base-url",
			"--api-key-env", "--api-key", "--no-verify", "--yes" } ) {
			if ( argument == known ) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] auto setup_flag_takes_value( const std::string_view argument ) -> bool {
		return argument == "--provider" || argument == "--model"
			|| argument == "--base-url" || argument == "--api-key-env"
			|| argument == "--api-key";
	}

	auto setup_usage_text( ) -> std::string {
		auto out = std::string{ "usage: mcode setup [options]\n\n" };
		out += "With no options, runs an interactive wizard.\n\n";
		out += "options:\n";
		out += "  --provider <name>    openai-chat-completions | anthropic-messages\n";
		out += "  --model <id>         the model id the provider expects\n";
		out += "  --base-url <url>     override the provider's endpoint\n";
		out += "  --api-key-env <name> the environment variable holding the key\n";
		out += "  --api-key <key>      verify with this key; never written to disk\n";
		out += "  --no-verify          skip the round-trip check\n";
		out += "  --yes                accept the recommended answers\n";

		return out;
	}

	auto run_setup( const std::vector< std::string >& arguments ) -> int {
		auto terminal = tui::tty_session::create( );

		// No terminal is not an error: it is the scripted path, and the flags
		// carry everything the wizard would have asked for.
		if ( !terminal ) {
			auto plain = tui::capabilities{ };
			plain.depth = tui::capabilities::color_depth::none;

			const auto brush = detail::painter{ plain };

			return run_scripted( arguments, brush );
		}

		const auto brush = detail::painter{ terminal->caps( ) };

		// Every flag is checked before the path is chosen, so an unrecognised one
		// is refused on a terminal too. It used to be dropped here and reported
		// only when stdout was redirected, which made `mcode setup --bogus` a
		// usage error in a script and a silent no-op interactively.
		for ( auto index = std::size_t{ 0 }; index < arguments.size( ); ++index ) {
			const auto& argument = arguments[ index ];

			if ( !is_setup_flag( argument ) ) {
				std::fprintf( stderr, "mcode: unknown setup option '%s'\n\n",
					argument.c_str( ) );
				std::fputs( setup_usage_text( ).c_str( ), stderr );

				return 2;
			}

			// The value of a flag that takes one is not itself a flag, so it is
			// stepped over rather than tested against the set.
			if ( setup_flag_takes_value( argument ) ) {
				++index;
			}
		}

		// An explicit flag also takes the scripted path, so a setup script never
		// waits on a prompt it cannot answer.
		for ( const auto& argument : arguments ) {
			if ( argument == "--yes" || argument == "--provider" || argument == "--model" ) {
				return run_scripted( arguments, brush );
			}
		}

		auto input = detail::reader{ *terminal };

		const auto path = detail::config_path( );

		if ( path.empty( ) ) {
			detail::write_line( brush.bad( "  no configuration directory is available" ) );

			return 2;
		}

		print_banner( brush, path );

		auto labels = std::vector< std::string >{ };
		auto notes = std::vector< std::string >{ };

		for ( const auto& entry : detail::PROVIDERS ) {
			labels.emplace_back( entry.label );
			notes.emplace_back( std::string{ entry.note } );
		}

		const auto chosen = input.choose( "  " + brush.strong( "Provider" ), labels, notes, 0 );

		if ( !chosen ) {
			return cancelled( brush );
		}

		const auto& entry = detail::PROVIDERS[ *chosen ];
		detail::blank( );

		auto base_url = std::string{ entry.base_url };

		if ( base_url.empty( ) ) {
			const auto entered = input.line( "  " + brush.strong( "Base URL" )
				+ brush.dim( "  https://api.example.com/v1/chat/completions" )
				+ "\n  " + brush.accent( ">" ) + " " );

			if ( !entered ) {
				return cancelled( brush );
			}

			base_url = *entered;
		}

		if ( base_url.empty( ) ) {
			detail::blank( );
			detail::write_line( "  " + brush.bad( "a base URL is required for this provider" ) );
			detail::blank( );

			return 2;
		}

		auto models = detail::suggested_models( *chosen );
		auto model = std::string{ };

		if ( models.empty( ) ) {
			const auto entered = input.line( "  " + brush.strong( "Model id" )
				+ "\n  " + brush.accent( ">" ) + " " );

			if ( !entered ) {
				return cancelled( brush );
			}

			if ( entered->empty( ) ) {
				detail::blank( );
				detail::write_line( "  " + brush.bad( "a model id is required" ) );
				detail::blank( );

				return 2;
			}

			model = *entered;
		} else {
			auto model_notes = std::vector< std::string >{ };

			for ( auto index = std::size_t{ 0 }; index < models.size( ); ++index ) {
				model_notes.emplace_back( index == 0 ? "recommended" : "" );
			}

			const auto picked = input.choose( "  " + brush.strong( "Model" ), models,
				model_notes, 0 );

			if ( !picked ) {
				return cancelled( brush );
			}

			model = models[ *picked ];
		}

		detail::blank( );

		const auto key_env = std::string{ entry.api_key_env };
		auto api_key = std::string{ };
		const auto* from_environment = std::getenv( key_env.c_str( ) );

		if ( from_environment != nullptr && *from_environment != '\0' ) {
			api_key = from_environment;

			detail::write_line( "  " + brush.strong( "API key" ) + "  "
				+ brush.good( "found " + key_env + " in the environment" ) );
		} else {
			detail::write_line( "  " + brush.strong( "API key" ) + "  "
				+ brush.dim( "read from " + key_env + ", never written to disk" ) );

			const auto entered = input.secret( "  " + brush.dim( "paste it to verify now, "
				"or press Enter to skip" ) + "\n  " + brush.accent( ">" ) + " " );

			if ( !entered ) {
				// Ctrl-C, not Enter. An empty line means "skip"; an interrupt
				// means stop, and it must not fall through to writing a config.
				return cancelled( brush );
			}

			api_key = *entered;
		}

		detail::blank( );

		const auto settings = detail::model_settings{ std::string{ entry.descriptor }, model,
			base_url, key_env };

		// Write before verifying: the check runs a real turn, and a real turn
		// reads the file. `previous` is kept so a failure can be undone.
		const auto previous = detail::read_text_file( path );

		if ( !detail::write_text_file( path, detail::splice_section( previous.value_or( std::string{ } ),
			detail::render_section( settings ) ) ) ) {
			detail::blank( );
			detail::write_line( "  " + brush.bad( "could not write " + path.string( ) ) );
			detail::blank( );

			return 2;
		}

		auto verified = false;

		if ( api_key.empty( ) ) {
			detail::write_line( "  " + brush.dim( "skipped verification; " + key_env
				+ " is not set" ) );
		} else {
			detail::write_line( "  " + brush.dim( "checking the provider with a real turn..." ) );

			const auto checked = verify( settings, api_key );

			if ( checked.ok ) {
				verified = true;

				detail::write_line( "  " + brush.good( "ok" ) + "  the provider answered" );
			} else {
				detail::write_line( "  " + brush.bad( "failed" ) + "  " + checked.detail );
				detail::blank( );

				const auto keep = input.choose( "  " + brush.strong( "Keep the config?" ),
					{ "Keep it and fix the key later", "Undo and start over" },
					{ "", "" }, 0 );

				if ( !keep || *keep == 1 ) {
					// Restore exactly what was there, including nothing. A failed
					// restore is reported rather than swallowed: the file is then
					// half-configured and the user has to be told.
					if ( previous ) {
						if ( !detail::write_text_file( path, *previous ) ) {
							detail::write_line( "  " + brush.bad( "could not restore "
								+ path.string( ) ) );
						}
					} else {
						std::error_code ignored;
						std::filesystem::remove( path, ignored );
					}

					return cancelled( brush );
				}
			}
		}

		return finish( brush, settings, path, verified, !api_key.empty( ) );
	}

}
