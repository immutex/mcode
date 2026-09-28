#include "mcode/tools/exec_policy.hxx"

#include <array>

namespace mcode::tools {

	namespace {

		inline constexpr auto SHELL_METACHARS = std::array{ '|', ';', '&', '`' };

		inline constexpr auto EXEC_RUNNERS = std::array< std::string_view, 6 >{
			"sh", "bash", "cmd", "cmd.exe", "xargs", "powershell",
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

		[[nodiscard]] auto is_substitution( const std::string_view token ) noexcept -> bool {
			return token.find( "$(" ) != std::string_view::npos ||
				token.find( "%VAR%" ) != std::string_view::npos;
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

		if ( yolo ) {
			return exec_decision::allow;
		}

		const auto& program = argv.front( );

		if ( list_contains( deny_argv, program ) ) {
			return exec_decision::deny;
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
		for ( const auto runner : EXEC_RUNNERS ) {
			if ( program == runner ) {
				return true;
			}
		}

		return false;
	}

}
