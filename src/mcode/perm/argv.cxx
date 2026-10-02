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

