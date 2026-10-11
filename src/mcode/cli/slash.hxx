#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/mcp/connect.hxx"
#include "mcode/model/types.hxx"
#include "mcode/tui/mention.hxx"
#include "mcode/tui/palette.hxx"

namespace mcode::cli {

	[[nodiscard]] auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >&;

	auto refresh_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& commands,
		std::string_view input ) -> void;

	// Fills the same palette with the mention picker's rows.
	auto refresh_mention_palette( mcode::tui::slash_palette& palette,
		const mcode::tui::mention_index& files, std::string_view input ) -> void;

	inline constexpr std::size_t HISTORY_LIMIT = 8;

	// Newest first: prefix, then substring, then subsequence.
	[[nodiscard]] auto rank_history( const std::vector< std::string >& entries,
		std::string_view query, std::size_t limit ) -> std::vector< std::string >;

	auto refresh_history_palette( mcode::tui::slash_palette& palette,
		const std::vector< std::string >& entries, std::string_view query ) -> void;

	// The row's name is `resume <id>`, so submitting it runs exactly the command
	// that id names; the description carries when it ran, its size and its prompt.
	[[nodiscard]] auto session_rows( const std::vector< session_ref >& sessions )
		-> std::vector< mcode::tui::slash_command >;

	auto refresh_sessions_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& rows, std::string_view query ) -> void;

	enum class palette_source {
		commands,
		mentions,
		history,
		sessions,
	};

	// Only a command runs: a session row is a `/resume <id>` line, while a mention
	// and a history entry are inserted into the prompt for the user to edit.
	[[nodiscard]] constexpr auto submits( const palette_source source ) noexcept -> bool {
		return source == palette_source::commands || source == palette_source::sessions;
	}

	[[nodiscard]] auto inserted_line( palette_source source, std::string_view input,
		const mcode::tui::slash_command& row ) -> std::string;

	// Pointers, so the caller keeps each container alive for as long as the controller.
	struct palette_sources {
		const std::vector< mcode::tui::slash_command >* commands = nullptr;
		const mcode::tui::mention_index* files = nullptr;
		const std::vector< std::string >* history = nullptr;

		// Owned by the caller: `list_sessions` returns a fresh vector each call.
		const std::vector< mcode::tui::slash_command >* sessions = nullptr;
	};

	class palette_controller {
	public:
		explicit palette_controller( palette_sources sources ) : sources_( sources ) { }

		auto refresh( mcode::tui::slash_palette& palette, std::string_view input )
			-> palette_source;

		auto open_history( mcode::tui::slash_palette& palette, std::string_view query )
			-> palette_source;

		auto open_sessions( mcode::tui::slash_palette& palette, std::string_view query )
			-> palette_source;

		[[nodiscard]] auto source( ) const noexcept -> palette_source { return source_; }

		auto reset( ) noexcept -> void { source_ = palette_source::commands; }

	private:
		palette_sources sources_;
		palette_source source_ = palette_source::commands;
	};

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

		bool open_mention = false;

		// A prompt the command wants run as a real turn, as if the user had typed it.
		// The REPL echoes the command, not this: it is instructions, not a transcript line.
		std::optional< std::string > submit_prompt;

		// The REPL performs the swap: `run_command` takes a `const agent_loop&`.
		std::optional< std::string > adopt_session_id;

		bool open_session_picker = false;

		bool start_new_session = false;

		std::string output;
	};

	[[nodiscard]] auto render_transcript_markdown(
		const std::vector< mcode::model::message >& history ) -> std::string;

	// Pure: the report is the whole input, so the rendering is testable without loading
	// an extension.
	[[nodiscard]] auto extensions_text( const mcode::ext::load_report& report ) -> std::string;

	// `/prompt`: lists what the connected servers offer, or fills one and hands the
	// text back for the REPL to submit as the next turn. `submit` is set only on the
	// fill path, so the caller can tell the two apart without a second call.
	[[nodiscard]] auto prompt_text( const mcode::mcp::server_set* servers,
		std::string_view arguments, std::optional< std::string >& submit ) -> std::string;

	// Runs a command an extension registered. The REPL supplies it so this file stays
	// free of the extension loader; null means no extensions are loaded.
	using extension_command_invoker = std::function< result< std::string >(
		std::string_view name, std::string_view arguments ) >;

	// The builtin table plus every command the loaded extensions registered, so the
	// palette lists them and `/help` names them.
	[[nodiscard]] auto all_commands( const mcode::ext::command_registry& extensions )
		-> std::vector< mcode::tui::slash_command >;

	// May be null when no session was built; `/extensions` reports that rather than an
	// empty list, because "nothing loaded" and "nothing to read" are different answers.
	[[nodiscard]] auto run_command( const command_match& match, const agent_loop& loop,
		const std::vector< mcode::tui::slash_command >& commands,
		const mcode::ext::load_report* extensions = nullptr,
		const std::filesystem::path* workspace = nullptr,
		const extension_command_invoker* invoke_extension = nullptr,
		const mcode::mcp::server_set* mcp_servers = nullptr ) -> command_result;

}
