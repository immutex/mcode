#include "mcode/perm/argv.hxx"
#include "mcode/support/text.hxx"

#include <array>

namespace mcode::perm {

	namespace {

		// `>` writes where the file tools are forbidden; a newline is a command separator.
		inline constexpr auto SHELL_METACHARS = std::array{ '|', ';', '&', '`', '<', '>', '\n', '\r' };

		inline constexpr auto EXEC_RUNNERS = std::array< std::string_view, 10 >{
			"sh", "sh.exe", "bash", "bash.exe", "cmd", "cmd.exe",
			"xargs", "powershell", "powershell.exe", "pwsh",
		};

		// a leading wrapper hides the program that actually runs, so it is stripped first.
		inline constexpr auto WRAPPER_PROGRAMS = std::array< std::string_view, 8 >{
			"env", "env.exe", "timeout", "timeout.exe", "nice", "nice.exe",
			"nohup", "nohup.exe",
		};

		inline constexpr std::string_view ENV_ASSIGNMENT = "=";

		[[nodiscard]] auto is_wrapper( const std::string_view program ) -> bool {
			const auto name = mcode::text::program_basename( program );

			for ( const auto wrapper : WRAPPER_PROGRAMS ) {
				if ( name == wrapper ) {
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] auto is_env_assignment( const std::string_view token ) -> bool {
			return token.find( ENV_ASSIGNMENT ) != std::string_view::npos;
		}

		[[nodiscard]] auto is_timeout( const std::string_view program ) -> bool {
			const auto name = mcode::text::program_basename( program );

			return name == "timeout" || name == "timeout.exe";
		}

		[[nodiscard]] auto flag_operand_count( const std::string_view program,
			const std::string_view flag ) -> std::size_t {
			const auto name = mcode::text::program_basename( program );

			if ( name == "env" || name == "env.exe" ) {
				return flag == "-u" || flag == "-C" || flag == "-S" ? 1 : 0;
			}

			if ( is_timeout( program ) ) {
				return flag == "-k" || flag == "-s" ? 1 : 0;
			}

			if ( name == "nice" || name == "nice.exe" ) {
				return flag == "-n" ? 1 : 0;
			}

			return 0;
		}

		// sh escapes exactly these inside double quotes; every other backslash is
		// literal, which is what keeps a Windows path intact.
		[[nodiscard]] auto is_escaped_in_double_quotes( const char character ) noexcept -> bool {
			return character == '"' || character == '\\' || character == '$' ||
				character == '`' || character == '\n';
		}

		[[nodiscard]] auto is_metachar( const char character ) noexcept -> bool {
			for ( const auto meta : SHELL_METACHARS ) {
				if ( character == meta ) {
					return true;
				}
			}

			return false;
		}

	}

	auto parse_command_line( const std::string_view command )
		-> std::optional< std::vector< std::string > > {
		auto tokens = std::vector< std::string >{ };
		auto index = std::size_t{ 0 };

		while ( index < command.size( ) ) {
			while ( index < command.size( ) &&
				( command[ index ] == ' ' || command[ index ] == '\t' ) ) {
				++index;
			}

			if ( index >= command.size( ) ) {
				break;
			}

			auto token = std::string{ };

			while ( index < command.size( ) && command[ index ] != ' ' && command[ index ] != '\t' ) {
				const auto character = command[ index ];

				if ( character == '"' || character == '\'' ) {
					const auto quote = character;
					++index;
					auto closed = false;

					while ( index < command.size( ) ) {
						if ( quote == '"' && command[ index ] == '\\' &&
							index + 1 < command.size( ) &&
							is_escaped_in_double_quotes( command[ index + 1 ] ) ) {
							token += command[ index + 1 ];
							index += 2;

							continue;
						}

						if ( command[ index ] == quote ) {
							closed = true;
							++index;

							break;
						}

						token += command[ index ];
						++index;
					}

					if ( !closed ) {
						return std::nullopt;
					}

					continue;
				}

				// Only an unquoted metacharacter means the model expected a shell; a
				// quoted one is literal data the program receives.
				if ( is_metachar( character ) || character == '$' || character == '%' ) {
					return std::nullopt;
				}

				token += character;
				++index;
			}

			tokens.push_back( std::move( token ) );
		}

		if ( tokens.empty( ) ) {
			return std::nullopt;
		}

		return tokens;
	}

	auto is_exec_runner( const std::string_view program ) noexcept -> bool {
		// `/bin/sh`, `C:\...\cmd.exe` and `CMD` must all land on the runner list.
		const auto name = mcode::text::program_basename( program );

		for ( const auto runner : EXEC_RUNNERS ) {
			if ( name == runner ) {
				return true;
			}
		}

		return false;
	}

	auto unwrap_command( const std::vector< std::string >& argv )
		-> std::vector< std::string > {
		auto index = std::size_t{ 0 };

		while ( index < argv.size( ) && is_wrapper( argv[ index ] ) ) {
			const auto program = argv[ index ];

			++index;

			while ( index < argv.size( ) ) {
				const auto& token = argv[ index ];

				if ( is_env_assignment( token ) || token == "--" ) {
					++index;

					continue;
				}

				if ( !token.starts_with( '-' ) ) {
					break;
				}

				index += 1 + flag_operand_count( program, token );
			}

			// `timeout` takes exactly one duration operand ahead of the program it runs.
			if ( is_timeout( program ) && index < argv.size( ) ) {
				++index;
			}
		}

		if ( index >= argv.size( ) ) {
			return { };
		}

		return std::vector< std::string >{ argv.begin( ) +
			static_cast< std::ptrdiff_t >( index ), argv.end( ) };
	}

}

