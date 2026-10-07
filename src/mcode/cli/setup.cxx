#include "mcode/cli/setup.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/tui/cell.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli {

	namespace {

		using tui::token;

		// The `[model]` section this writes. One place, so the header it looks
		// for and the header it writes cannot drift apart.
		inline constexpr std::string_view SECTION_HEADER = "[model]";
		inline constexpr std::string_view CONFIG_FILE_NAME = "config.toml";

		// A verification turn that never returns is worse than a failed one.
		inline constexpr std::uint32_t SETUP_READ_TIMEOUT_MS = 300'000;
		inline constexpr std::int64_t VERIFY_TIMEOUT_MS = 90'000;

		inline constexpr std::size_t RULE_WIDTH = 62;
		inline constexpr std::size_t MAX_DIGIT_CHOICE = 9;

		// -------------------------------------------------------------------
		// Presentation. Every colour comes from the shipped theme, resolved at
		// the depth this terminal supports, so the wizard cannot drift from the
		// TUI palette and still reads on a 16-colour terminal.
		// -------------------------------------------------------------------

		class painter {
		public:
			explicit painter( const tui::capabilities& caps ) : caps_( caps ) { }

			[[nodiscard]] auto paint( const token colour, const std::string_view text,
				const bool bold = false ) const -> std::string {
				const auto code = tui::token_color( colour, caps_.depth );

				if ( code.empty( ) ) {
					return std::string{ text };
				}

				auto out = std::string{ "\x1b[0" };

				if ( bold ) {
					out += ";1";
				}

				out += ';';
				out += code;
				out += 'm';
				out += text;
				out += "\x1b[0m";

				return out;
			}

			[[nodiscard]] auto dim( const std::string_view text ) const -> std::string {
				return paint( token::muted, text );
			}

			[[nodiscard]] auto strong( const std::string_view text ) const -> std::string {
				return paint( token::text, text, true );
			}

			[[nodiscard]] auto good( const std::string_view text ) const -> std::string {
				return paint( token::success, text );
			}

			[[nodiscard]] auto bad( const std::string_view text ) const -> std::string {
				return paint( token::error, text );
			}

			[[nodiscard]] auto mark( const std::string_view text ) const -> std::string {
				return paint( token::accent, text, true );
			}

			[[nodiscard]] auto accent( const std::string_view text ) const -> std::string {
				return paint( token::accent, text );
			}

		private:
			tui::capabilities caps_;
		};

		auto write_line( const std::string& text ) -> void {
			std::fputs( text.c_str( ), stdout );
			std::fputc( '\n', stdout );
			std::fflush( stdout );
		}

		auto blank( ) -> void {
			std::fputc( '\n', stdout );
			std::fflush( stdout );
		}

		// -------------------------------------------------------------------
		// Input. One reader, so the interactive and scripted paths behave the
		// same and a closed stdin is reported rather than read as empty.
		// -------------------------------------------------------------------

		class reader {
		public:
			explicit reader( tui::tty_session& terminal ) : terminal_( terminal ) { }

			[[nodiscard]] auto line( const std::string& prompt ) -> std::optional< std::string > {
				std::fputs( prompt.c_str( ), stdout );
				std::fflush( stdout );

				auto value = terminal_.read_line( SETUP_READ_TIMEOUT_MS );

				if ( !value ) {
					blank( );
				}

				return value;
			}

			// An arrow-key menu. The highlight starts on `preselected`, so Enter
			// takes the recommended answer, and a digit jumps straight to a row.
			[[nodiscard]] auto choose( const std::string& title,
				const std::vector< std::string >& options,
				const std::vector< std::string >& notes, const std::size_t preselected )
				-> std::optional< std::size_t > {
				auto selected = std::min( preselected, options.size( ) - 1 );

				write_line( title );
				blank( );

				const auto draw = [&]( ) -> void {
					for ( auto index = std::size_t{ 0 }; index < options.size( ); ++index ) {
						const auto active = index == selected;
						auto row = std::string{ "   " };

						row += active ? accent( ">" ) : " ";
						row += ' ';
						row += active ? strong( options[ index ] ) : options[ index ];

						if ( index < notes.size( ) && !notes[ index ].empty( ) ) {
							row += "  ";
							row += dim( notes[ index ] );
						}

						write_line( row );
					}
				};

				draw( );

				const auto drawn = options.size( );

				for ( ;; ) {
					const auto key = terminal_.read_key( SETUP_READ_TIMEOUT_MS );

					if ( key.type == tui::key_event::kind::exit
						|| key.type == tui::key_event::kind::interrupt ) {
						return std::nullopt;
					}

					if ( key.type == tui::key_event::kind::enter ) {
						return selected;
					}

					if ( key.type == tui::key_event::kind::timeout ) {
						continue;
					}

					if ( key.type == tui::key_event::kind::up ) {
						if ( selected == 0 ) {
							continue;
						}

						--selected;
					} else if ( key.type == tui::key_event::kind::down ) {
						if ( selected + 1 >= options.size( ) ) {
							continue;
						}

						++selected;
					} else if ( key.type == tui::key_event::kind::character
						&& !key.text.empty( ) ) {
						const auto digit = key.text.front( );
						const auto limit = static_cast< char >( '0' + MAX_DIGIT_CHOICE );

						if ( digit < '1' || digit > limit ) {
							continue;
						}

						const auto index = static_cast< std::size_t >( digit - '1' );

						if ( index >= options.size( ) || index == selected ) {
							continue;
						}

						selected = index;
					} else {
						continue;
					}

					// Move back over the menu and repaint it, rather than clearing
					// the screen, so the banner stays visible.
					std::fprintf( stdout, "\x1b[%zuA", drawn );
					std::fflush( stdout );

					draw( );
				}
			}

			// A secret is read the same way and simply never echoed back.
			[[nodiscard]] auto secret( const std::string& prompt )
				-> std::optional< std::string > {
				return line( prompt );
			}

		private:
			[[nodiscard]] auto styled( const token colour, const std::string_view text,
				const bool bold ) const -> std::string {
				const auto code = tui::token_color( colour, terminal_.caps( ).depth );

				if ( code.empty( ) ) {
					return std::string{ text };
				}

				auto out = std::string{ "\x1b[0" };

				if ( bold ) {
					out += ";1";
				}

				out += ';';
				out += code;
				out += 'm';
				out += text;
				out += "\x1b[0m";

				return out;
			}

			[[nodiscard]] auto strong( const std::string_view text ) const -> std::string {
				return styled( token::text, text, true );
			}

			[[nodiscard]] auto dim( const std::string_view text ) const -> std::string {
				return styled( token::muted, text, false );
			}

			[[nodiscard]] auto accent( const std::string_view text ) const -> std::string {
				return styled( token::accent, text, true );
			}

			tui::tty_session& terminal_;
		};

		// -------------------------------------------------------------------
		// The provider catalogue. Endpoints and key names mirror the shipped
		// provider descriptors, so a config written here is one the extension
		// already knows how to talk to.
		// -------------------------------------------------------------------

		struct provider_choice {
			std::string_view label;
			std::string_view descriptor;
			std::string_view base_url;
			std::string_view api_key_env;
			std::string_view note;
		};

		inline constexpr auto PROVIDERS = std::array{
			provider_choice{ "OpenAI", "openai-chat-completions",
				"https://api.openai.com/v1/chat/completions", "OPENAI_API_KEY",
				"platform.openai.com" },
			provider_choice{ "Anthropic", "anthropic-messages",
				"https://api.anthropic.com/v1/messages", "ANTHROPIC_API_KEY",
				"console.anthropic.com" },
			provider_choice{ "OpenAI-compatible endpoint", "openai-chat-completions",
				"", "MCODE_API_KEY", "any /v1/chat/completions gateway" },
		};

		// Only the models the compiled-in table prices. A model outside it needs
		// a `[models."id"]` block, which the wizard names rather than silently
		// writing a config that would refuse to run.
		[[nodiscard]] auto suggested_models( const std::size_t provider_index )
			-> std::vector< std::string > {
			switch ( provider_index ) {
				case 0: return { "gpt-5", "gpt-5-mini" };
				default: return { };
			}
		}

		// -------------------------------------------------------------------
		// Config writing. The file is edited as text: the `[model]` section is
		// replaced or appended and everything else, including the
		// `[models."..."]` pricing blocks and their comments, is preserved byte
		// for byte. Parsing and re-serialising would need a TOML writer that
		// does not exist, and would drop the rationale those comments carry.
		// -------------------------------------------------------------------

		[[nodiscard]] auto escape_toml( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			for ( const auto character : text ) {
				switch ( character ) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;

					default: out.push_back( character ); break;
				}
			}

			return out;
		}

		struct model_settings {
			std::string provider;
			std::string model;
			std::string base_url;
			std::string api_key_env;
		};

		[[nodiscard]] auto render_section( const model_settings& settings ) -> std::string {
			auto out = std::string{ };
			out += SECTION_HEADER;
			out += "\nprovider = \"";
			out += escape_toml( settings.provider );
			out += "\"\nmodel = \"";
			out += escape_toml( settings.model );
			out += "\"\n";

			if ( !settings.base_url.empty( ) ) {
				out += "base_url = \"";
				out += escape_toml( settings.base_url );
				out += "\"\n";
			}

			out += "api_key_env = \"";
			out += escape_toml( settings.api_key_env );
			out += "\"\n";

			return out;
		}

		// Replaces the `[model]` section in place, or appends one.
		//
		// Line-based rather than offset-based, because a config edited on Windows
		// carries CRLF endings and a check for a bare `\n` after the header would
		// miss it and append a SECOND `[model]` section. Two sections with the
		// same name are a duplicate-key error at load, so that failure would land
		// on the user's next start rather than here.
		[[nodiscard]] auto splice_section( const std::string_view existing,
			const std::string& section ) -> std::string {
			// Split preserving nothing; the ending is reapplied from the original
			// dominant style so the file is not half-converted.
			auto lines = std::vector< std::string >{ };
			auto endings = std::string{ };
			auto cursor = std::size_t{ 0 };

			while ( cursor <= existing.size( ) && !existing.empty( ) ) {
				const auto next = existing.find( '\n', cursor );

				if ( next == std::string_view::npos ) {
					if ( cursor < existing.size( ) ) {
						lines.emplace_back( existing.substr( cursor ) );
					}

					break;
				}

				auto line = std::string{ existing.substr( cursor, next - cursor ) };

				if ( !line.empty( ) && line.back( ) == '\r' ) {
					line.pop_back( );

					if ( endings.empty( ) ) {
						endings = "\r\n";
					}
				} else if ( endings.empty( ) ) {
					endings = "\n";
				}

				lines.push_back( std::move( line ) );
				cursor = next + 1;
			}

			if ( endings.empty( ) ) {
				endings = "\n";
			}

			// The replacement, split the same way so it joins the same file.
			auto replacement = std::vector< std::string >{ };
			auto section_cursor = std::size_t{ 0 };

			while ( section_cursor <= section.size( ) ) {
				const auto next = section.find( '\n', section_cursor );

				if ( next == std::string::npos ) {
					break;
				}

				replacement.emplace_back( section.substr( section_cursor, next - section_cursor ) );
				section_cursor = next + 1;
			}

			const auto is_header = []( const std::string& line ) -> bool {
				return !line.empty( ) && line.front( ) == '[' && line.back( ) == ']';
			};

			const auto start = [&]( ) -> std::size_t {
				for ( auto index = std::size_t{ 0 }; index < lines.size( ); ++index ) {
					if ( lines[ index ] == SECTION_HEADER ) {
						return index;
					}
				}

				return lines.size( );
			}( );

			if ( start == lines.size( ) ) {
				// Not present: append, with a blank separator when the file is not empty.
				if ( !lines.empty( ) && !lines.back( ).empty( ) ) {
					lines.emplace_back( std::string{ } );
				}

				lines.insert( lines.end( ), replacement.begin( ), replacement.end( ) );
			} else {
				// The section runs to the next header, or to the end of the file.
				auto end = start + 1;

				while ( end < lines.size( ) && !is_header( lines[ end ] ) ) {
					++end;
				}

				lines.erase( lines.begin( ) + static_cast< std::ptrdiff_t >( start ),
					lines.begin( ) + static_cast< std::ptrdiff_t >( end ) );
				lines.insert( lines.begin( ) + static_cast< std::ptrdiff_t >( start ),
					replacement.begin( ), replacement.end( ) );
			}

			auto out = std::string{ };

			for ( const auto& line : lines ) {
				out += line;
				out += endings;
			}

			return out;
		}

		[[nodiscard]] auto read_text_file( const std::filesystem::path& path )
			-> std::optional< std::string > {
			if ( !std::filesystem::exists( path ) ) {
				return std::string{ };
			}

			auto* file = std::fopen( path.string( ).c_str( ), "rb" );

			if ( file == nullptr ) {
				return std::nullopt;
			}

			auto out = std::string{ };
			auto buffer = std::array< char, 4096 >{ };
			auto count = std::size_t{ 0 };

			while ( ( count = std::fread( buffer.data( ), 1, buffer.size( ), file ) ) > 0 ) {
				out.append( buffer.data( ), count );
			}

			std::fclose( file );

			return out;
		}

		// Writes beside the target and renames, so an interrupted run cannot
		// leave a half-written config that the next start would refuse.
		[[nodiscard]] auto write_text_file( const std::filesystem::path& path,
			const std::string_view content ) -> bool {
			auto error = std::error_code{ };

			if ( !path.parent_path( ).empty( ) ) {
				std::filesystem::create_directories( path.parent_path( ), error );

				if ( error ) {
					return false;
				}
			}

			const auto temporary = path.string( ) + ".new";
			auto* file = std::fopen( temporary.c_str( ), "wb" );

			if ( file == nullptr ) {
				return false;
			}

			const auto written = std::fwrite( content.data( ), 1, content.size( ), file );
			std::fclose( file );

			if ( written != content.size( ) ) {
				std::remove( temporary.c_str( ) );

				return false;
			}

			std::filesystem::rename( temporary, path, error );

			if ( !error ) {
				return true;
			}

			// A rename onto a file that is open fails on Windows; a copy still
			// completes the write rather than losing the user's settings.
			std::filesystem::copy_file( temporary, path,
				std::filesystem::copy_options::overwrite_existing, error );
			std::remove( temporary.c_str( ) );

			return !error;
		}

		[[nodiscard]] auto config_path( ) -> std::filesystem::path {
			const auto directory = mcode::platform::app_data_path(
				mcode::platform::data_kind::config );

			if ( !directory ) {
				return std::filesystem::path{ };
			}

			return *directory / CONFIG_FILE_NAME;
		}

		// -------------------------------------------------------------------
		// Verification. The strongest check available is the one the user is
		// about to run: write the config, then run a real turn with it. That
		// exercises the descriptor, the endpoint, the credential and the
		// streaming parser, so a success here is the whole path working rather
		// than a hand-rolled request that might not match it.
		// -------------------------------------------------------------------

		[[nodiscard]] auto self_path( ) -> std::filesystem::path {
			const auto directory = mcode::platform::executable_directory( );

			if ( !directory ) {
				return std::filesystem::path{ };
			}

			for ( const auto name : { "mcode", "mcode.exe" } ) {
				const auto candidate = *directory / name;

				if ( std::filesystem::exists( candidate ) ) {
					return candidate;
				}
			}

			return { };
		}

		struct verification {
			bool ok = false;
			std::string detail;
		};

		[[nodiscard]] auto verify( const model_settings& settings,
			const std::string& api_key ) -> verification {
			const auto self = self_path( );

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
		auto print_banner( const painter& brush, const std::filesystem::path& path ) -> void {
			blank( );
			write_line( "  " + brush.mark( "mcode setup" ) );
			write_line( "  " + brush.dim( "Choose a provider. Writes one file: "
				+ path.string( ) ) );
			blank( );
		}

		[[nodiscard]] auto finish( const painter& brush, const model_settings& settings,
			const std::filesystem::path& path, const bool verified,
			const bool have_key ) -> int {
			blank( );
			write_line( "  " + brush.dim( std::string( RULE_WIDTH, '-' ) ) );
			blank( );
			write_line( "  " + brush.good( "wrote" ) + "  " + path.string( ) );
			write_line( "  " + brush.dim( "provider  " ) + settings.provider );
			write_line( "  " + brush.dim( "model     " ) + settings.model );

			if ( !settings.base_url.empty( ) ) {
				write_line( "  " + brush.dim( "endpoint  " ) + settings.base_url );
			}

			write_line( "  " + brush.dim( "key from  " ) + settings.api_key_env
				+ ( verified ? brush.good( "  verified" ) : std::string{ } ) );
			blank( );

			if ( !have_key ) {
				write_line( "  " + brush.strong( "Next" ) + "  export "
					+ brush.accent( settings.api_key_env ) + "=your-key" );
			}

			write_line( "  " + brush.strong( "Next" ) + "  run " + brush.mark( "mcode" ) );
			blank( );

			return 0;
		}

		[[nodiscard]] auto cancelled( const painter& brush ) -> int {
			blank( );
			write_line( "  " + brush.dim( "cancelled; nothing was written" ) );
			blank( );

			return 1;
		}

		// Scripted: every value comes from a flag, and a missing one is a hard
		// failure rather than a silent default.
		[[nodiscard]] auto run_scripted( const std::vector< std::string >& arguments,
			const painter& brush ) -> int {
			auto provider = std::string{ };
			auto model = std::string{ };
			auto base_url = std::string{ };
			auto key_env = std::string{ };
			auto api_key = std::string{ };
			auto should_verify = true;

			for ( auto index = std::size_t{ 0 }; index < arguments.size( ); ++index ) {
				const auto& argument = arguments[ index ];

				const auto value_for = [&]( const std::string_view flag ) -> std::string {
					if ( index + 1 >= arguments.size( ) ) {
						std::fprintf( stderr, "mcode: %s needs a value\n",
							std::string{ flag }.c_str( ) );
						std::exit( 2 );
					}

					return arguments[ ++index ];
				};

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

			const auto settings = model_settings{ provider, model, base_url, key_env };
			const auto path = config_path( );

			if ( path.empty( ) ) {
				std::fprintf( stderr, "mcode: no configuration directory is available\n" );

				return 2;
			}

			if ( !write_text_file( path, splice_section(
				read_text_file( path ).value_or( std::string{ } ),
				render_section( settings ) ) ) ) {
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

			write_line( brush.good( "wrote" ) + " " + path.string( ) );

			return 0;
		}

	}

	// The pure core of the config edit, exposed for tests: the section is built
	// here and spliced by the same code the wizard uses, so a test cannot pass
	// against a re-implementation that has drifted.
	auto splice_model_section( const std::string_view existing,
		const std::string_view provider, const std::string_view model,
		const std::string_view base_url, const std::string_view api_key_env ) -> std::string {
		const auto settings = model_settings{ std::string{ provider }, std::string{ model },
			std::string{ base_url }, std::string{ api_key_env } };

		return splice_section( existing, render_section( settings ) );
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

			const auto brush = painter{ plain };

			return run_scripted( arguments, brush );
		}

		const auto brush = painter{ terminal->caps( ) };

		// An explicit flag also takes the scripted path, so a setup script never
		// waits on a prompt it cannot answer.
		for ( const auto& argument : arguments ) {
			if ( argument == "--yes" || argument == "--provider" || argument == "--model" ) {
				return run_scripted( arguments, brush );
			}
		}

		auto input = reader{ *terminal };

		const auto path = config_path( );

		if ( path.empty( ) ) {
			write_line( brush.bad( "  no configuration directory is available" ) );

			return 2;
		}

		print_banner( brush, path );

		auto labels = std::vector< std::string >{ };
		auto notes = std::vector< std::string >{ };

		for ( const auto& entry : PROVIDERS ) {
			labels.emplace_back( entry.label );
			notes.emplace_back( std::string{ entry.note } );
		}

		const auto chosen = input.choose( "  " + brush.strong( "Provider" ), labels, notes, 0 );

		if ( !chosen ) {
			return cancelled( brush );
		}

		const auto& entry = PROVIDERS[ *chosen ];
		blank( );

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
			blank( );
			write_line( "  " + brush.bad( "a base URL is required for this provider" ) );
			blank( );

			return 2;
		}

		auto models = suggested_models( *chosen );
		auto model = std::string{ };

		if ( models.empty( ) ) {
			const auto entered = input.line( "  " + brush.strong( "Model id" )
				+ "\n  " + brush.accent( ">" ) + " " );

			if ( !entered ) {
				return cancelled( brush );
			}

			if ( entered->empty( ) ) {
				blank( );
				write_line( "  " + brush.bad( "a model id is required" ) );
				blank( );

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

		blank( );

		const auto key_env = std::string{ entry.api_key_env };
		auto api_key = std::string{ };
		const auto* from_environment = std::getenv( key_env.c_str( ) );

		if ( from_environment != nullptr && *from_environment != '\0' ) {
			api_key = from_environment;

			write_line( "  " + brush.strong( "API key" ) + "  "
				+ brush.good( "found " + key_env + " in the environment" ) );
		} else {
			write_line( "  " + brush.strong( "API key" ) + "  "
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

		blank( );

		const auto settings = model_settings{ std::string{ entry.descriptor }, model,
			base_url, key_env };

		// Write before verifying: the check runs a real turn, and a real turn
		// reads the file. `previous` is kept so a failure can be undone.
		const auto previous = read_text_file( path );

		if ( !write_text_file( path, splice_section( previous.value_or( std::string{ } ),
			render_section( settings ) ) ) ) {
			blank( );
			write_line( "  " + brush.bad( "could not write " + path.string( ) ) );
			blank( );

			return 2;
		}

		auto verified = false;

		if ( api_key.empty( ) ) {
			write_line( "  " + brush.dim( "skipped verification; " + key_env
				+ " is not set" ) );
		} else {
			write_line( "  " + brush.dim( "checking the provider with a real turn..." ) );

			const auto checked = verify( settings, api_key );

			if ( checked.ok ) {
				verified = true;

				write_line( "  " + brush.good( "ok" ) + "  the provider answered" );
			} else {
				write_line( "  " + brush.bad( "failed" ) + "  " + checked.detail );
				blank( );

				const auto keep = input.choose( "  " + brush.strong( "Keep the config?" ),
					{ "Keep it and fix the key later", "Undo and start over" },
					{ "", "" }, 0 );

				if ( !keep || *keep == 1 ) {
					// Restore exactly what was there, including nothing. A failed
					// restore is reported rather than swallowed: the file is then
					// half-configured and the user has to be told.
					if ( previous ) {
						if ( !write_text_file( path, *previous ) ) {
							write_line( "  " + brush.bad( "could not restore "
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
