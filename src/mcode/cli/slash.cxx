#include "mcode/cli/slash.hxx"

#include <algorithm>

namespace mcode::cli {

	auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >& {
		static const auto COMMANDS = std::vector< mcode::tui::slash_command >{
			{ "help", "List the available commands" },
			{ "cost", "Show tokens spent and spend so far" },
			{ "model", "Show the model this session runs" },
			{ "tools", "List the tools the agent can call" },
			{ "exit", "End the session" },
		};

		return COMMANDS;
	}

	auto refresh_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& commands,
		const std::string_view input ) -> void {
		const auto query = mcode::tui::command_query( input );

		if ( !query ) {
			palette.open = false;
			palette.matches.clear( );
			palette.selected = 0;

			return;
		}

		palette.open = true;
		palette.query = std::string{ *query };
		palette.matches = mcode::tui::filter_commands( commands, *query );

		if ( palette.selected >= palette.matches.size( ) ) {
			palette.selected = 0;
		}
	}

	auto match_command( const std::string_view line,
		const std::vector< mcode::tui::slash_command >& commands ) -> command_match {
		auto match = command_match{ };

		if ( !line.starts_with( '/' ) ) {
			return match;
		}

		match.is_command = true;

		const auto body = line.substr( 1 );
		const auto space = body.find( ' ' );
		match.name = body.substr( 0, space );
		match.arguments = space == std::string_view::npos
			? std::string_view{ } : body.substr( space + 1 );

		const auto found = std::find_if( commands.begin( ), commands.end( ),
			[ & ]( const mcode::tui::slash_command& entry ) { return entry.name == match.name; } );

		if ( found != commands.end( ) ) {
			match.entry = &*found;
		}

		return match;
	}

	namespace {

		[[nodiscard]] auto help_text( const std::vector< mcode::tui::slash_command >& commands )
			-> std::string {
			auto out = std::string{ };

			for ( const auto& entry : commands ) {
				out += "/" + entry.name + "  " + entry.description + '\n';
			}

			return out;
		}

	}

	auto run_command( const command_match& match, const agent_loop& loop,
		const std::vector< mcode::tui::slash_command >& commands ) -> command_result {
		auto result = command_result{ };

		if ( match.entry == nullptr ) {
			result.output = "unknown command: /" + std::string{ match.name } +
				"  (try /help)";

			return result;
		}

		if ( match.name == "exit" ) {
			result.should_exit = true;
		} else if ( match.name == "cost" ) {
			const auto& budget = loop.budget( );

			result.output = "tokens: " + std::to_string( budget.tokens_used ) +
				"   spend: $" + std::to_string( budget.usd_used );
		} else if ( match.name == "model" ) {
			result.output = std::string{ loop.model_name( ) };
		} else if ( match.name == "tools" ) {
			for ( const auto* tool : loop.registry( ).all( ) ) {
				if ( !result.output.empty( ) ) {
					result.output += ", ";
				}

				result.output += tool->name;
			}
		} else {
			result.output = help_text( commands );
		}

		return result;
	}

}
