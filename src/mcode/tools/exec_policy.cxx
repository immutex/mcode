#include "mcode/tools/exec_policy.hxx"

#include <array>

namespace mcode::tools {

	namespace {

		// Redirection and newline are command-shape, not token text: `>` writes
		// where the file tools are forbidden, and an embedded newline is a command
		// separator the gate would never see.
		inline constexpr auto SHELL_METACHARS = std::array{ '|', ';', '&', '`', '<', '>', '\n', '\r' };

		inline constexpr auto EXEC_RUNNERS = std::array< std::string_view, 10 >{
			"sh", "sh.exe", "bash", "bash.exe", "cmd", "cmd.exe",
			"xargs", "powershell", "powershell.exe", "pwsh",
		};

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

		// Both shells expand `$` and `%` forms; either in argv means the executed
		// text differs from the judged text. Both are rare in legitimate single
		// commands, so refusing outright is the cheap exact gate.
		[[nodiscard]] auto is_substitution( const std::string_view token ) noexcept -> bool {
			return token.find( '$' ) != std::string_view::npos ||
				token.find( '%' ) != std::string_view::npos;
		}

		[[nodiscard]] auto list_contains( const std::vector< std::string >& list,
			const std::string_view token ) noexcept -> bool {
			for ( const auto& entry : list ) {
				if ( entry == token ) {
					return true;
				}
			}

			return false;
		}

	}

	auto exec_policy::decide( const std::vector< std::string >& argv ) const -> exec_decision {
		if ( argv.empty( ) ) {
			return exec_decision::deny;
		}

		if ( is_exec_runner( argv.front( ) ) ) {
			return exec_decision::deny;
		}

		const auto& program = argv.front( );

		// Deny is checked before allow regardless of list order; the two ifs are
		// the precedence, not an accident of it.
		if ( list_contains( deny_argv, program ) ) {
			return exec_decision::deny;
		}

		if ( yolo ) {
			return exec_decision::allow;
		}

		if ( list_contains( allow_argv, program ) ) {
			return exec_decision::allow;
		}

		return exec_decision::deny;
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
		// Normalize the spelling the gate sees to the one the OS resolves: strip
		// any leading path (both separators), lowercase, then compare. `/bin/sh`,
		// `C:\...\cmd.exe` and `CMD` must all land on the runner list.
		auto name = std::string{ };

		for ( const auto character : program ) {
			if ( character == '/' || character == '\\' ) {
				name.clear( );

				continue;
			}

			name += character >= 'A' && character <= 'Z'
				? static_cast< char >( character - 'A' + 'a' )
				: character;
		}

		for ( const auto runner : EXEC_RUNNERS ) {
			if ( name == runner ) {
				return true;
			}
		}

		return false;
	}

}
