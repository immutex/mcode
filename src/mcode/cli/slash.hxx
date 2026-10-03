#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/model/types.hxx"
#include "mcode/tui/mention.hxx"
#include "mcode/tui/palette.hxx"

namespace mcode::cli {

	[[nodiscard]] auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >&;

	auto refresh_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& commands,
		std::string_view input ) -> void;

	// Fills the same palette with the mention picker's rows. One list widget,
	// three sources: only the gutter and the row names differ.
	auto refresh_mention_palette( mcode::tui::slash_palette& palette,
		const mcode::tui::mention_index& files, std::string_view input ) -> void;

	// Rows the history palette offers at most.
	inline constexpr std::size_t HISTORY_LIMIT = 8;

	// Ranks submitted entries against `query`, newest first: a prefix match,
	// then a substring, then a subsequence. An empty query is the most recent
	// entries, which is what Ctrl+R shows before anything is typed.
	[[nodiscard]] auto rank_history( const std::vector< std::string >& entries,
		std::string_view query, std::size_t limit ) -> std::vector< std::string >;

	// Fills the same palette from the editor's submitted history. `query` is
	// the reverse-search fragment itself, not an input line: history has no
	// sigil to strip.
	auto refresh_history_palette( mcode::tui::slash_palette& palette,
		const std::vector< std::string >& entries, std::string_view query ) -> void;

	// Which source fills the one palette: slash commands, the file picker, or
	// the session history. The widget, the renderer and the selection rules are
	// shared; only the rows and the prefix differ.
	enum class palette_source {
		commands,
		mentions,
		history,
	};

	// Only a slash command runs. A mention and a history entry are inserted
	// into the prompt instead, so neither is a submission.
	[[nodiscard]] constexpr auto submits( const palette_source source ) noexcept -> bool {
		return source == palette_source::commands;
	}

	// The text the highlighted row puts in the prompt. A command is completed
	// with room for arguments; a mention becomes `@path `; a history entry is
	// inserted as it was typed.
	[[nodiscard]] auto inserted_line( palette_source source, std::string_view input,
		const mcode::tui::slash_command& row ) -> std::string;

	// Everything a palette source needs. Pointers, so the caller keeps each
	// container alive for as long as the controller lives.
	struct palette_sources {
		const std::vector< mcode::tui::slash_command >* commands = nullptr;
		const mcode::tui::mention_index* files = nullptr;
		const std::vector< std::string >* history = nullptr;
	};

	// Owns which source the one palette is showing and fills it from that
	// source. Ctrl+R opens the history search; a typed '@' opens the file
	// picker; a mention that closes falls back to the commands. The history
	// search reads its query out of `input`, so typing filters it.
	class palette_controller {
	public:
		explicit palette_controller( palette_sources sources ) : sources_( sources ) { }

		// Fills `palette` from the active source, which may change because of
		// what `input` now contains.
		auto refresh( mcode::tui::slash_palette& palette, std::string_view input )
			-> palette_source;

		// Ctrl+R: the history search, seeded with whatever is typed.
		auto open_history( mcode::tui::slash_palette& palette, std::string_view query )
			-> palette_source;

		[[nodiscard]] auto source( ) const noexcept -> palette_source { return source_; }

		// Back to the commands: Esc, a submission, or a fresh prompt.
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

		// The mention picker changes the prompt instead of printing, so the
		// REPL opens it after the call returns rather than the command
		// reaching into the editor itself.
		bool open_mention = false;

		std::string output;
	};

	// The history as one Markdown document, which is what /export writes.
	// Pure, so it is testable without a live session.
	[[nodiscard]] auto render_transcript_markdown(
		const std::vector< mcode::model::message >& history ) -> std::string;

	[[nodiscard]] auto run_command( const command_match& match, const agent_loop& loop,
		const std::vector< mcode::tui::slash_command >& commands ) -> command_result;

}
