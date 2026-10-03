#include "cli_repl_plain.hxx"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "cli_session.hxx"
#include "mcode/cli/repl.hxx"

auto run_plain_repl( const std::vector< std::string >& arguments ) -> int {
	const auto plain_reader = []( ) -> std::optional< std::string > {
		auto line = std::string{ };

		if ( !std::getline( std::cin, line ) ) {
			return std::nullopt;
		}

		while ( !line.empty( ) && line.back( ) == '\r' ) {
			line.pop_back( );
		}

		return line;
	};

	return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
}
