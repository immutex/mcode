#include "mcode/perm/argv.hxx"

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

		[[nodiscard]] auto lower_ascii( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			for ( auto& character : out ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return out;
		}

		// path-stripped and lowercased, so `/usr/bin/timeout` and `TIMEOUT` both strip.
		[[nodiscard]] auto program_name( const std::string_view program ) -> std::string {
			const auto slash = program.find_last_of( "/\\" );
			const auto base = slash == std::string_view::npos ? program : program.substr( slash + 1 );

			return lower_ascii( base );
		}

		[[nodiscard]] auto is_wrapper( const std::string_view program ) -> bool {
			const auto name = program_name( program );

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
			const auto name = program_name( program );

			return name == "timeout" || name == "timeout.exe";
		}

		[[nodiscard]] auto flag_operand_count( const std::string_view program,
			const std::string_view flag ) -> std::size_t {
			const auto name = program_name( program );

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

		[[nodiscard]] auto contains_metachar( const std::string_view token ) noexcept -> bool {
			for ( const auto character : token ) {
				for ( const auto meta : SHELL_METACHARS ) {
					if ( character == meta ) {
						return true;
					}
				}
			}

			return false;
		}

		// both shells expand `$` and `%`, so the executed text would differ from the judged text.
		[[nodiscard]] auto is_substitution( const std::string_view token ) noexcept -> bool {
			return token.find( '$' ) != std::string_view::npos ||
				token.find( '%' ) != std::string_view::npos;
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

				token += character;
				++index;
			}

			if ( contains_metachar( token ) || is_substitution( token ) ) {
				return std::nullopt;
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
		const auto name = program_name( program );

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

			// `NAME=VALUE`, `--` and the wrapper's own flags, in whatever order they appear.
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

			// `timeout` takes exactly one duration operand before the program it runs.
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

