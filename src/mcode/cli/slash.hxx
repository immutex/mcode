#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/tui/palette.hxx"

namespace mcode::cli {

	[[nodiscard]] auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >&;

	auto refresh_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& commands,
		std::string_view input ) -> void;

	struct command_match {
		bool is_command = false;

		const mcode::tui::slash_command* entry = nullptr;
		std::string_view name;
		std::string_view arguments;
	};

	[[nodiscard]] auto match_command( std::string_view line,
		const std::vector< mcode::tui::slash_command >& commands ) -> command_match;

	struct command_result {
		bool should_exit = false;

		std::string output;
	};

	[[nodiscard]] auto run_command( const command_match& match, const agent_loop& loop,
		const std::vector< mcode::tui::slash_command >& commands ) -> command_result;

}
