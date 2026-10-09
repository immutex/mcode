#include "mcode/cli/slash.hxx"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "mcode/fs/snapshot.hxx"
#include "mcode/support/time.hxx"

namespace mcode::cli {

	namespace {


		inline constexpr std::uint64_t PERCENT_SCALE = 100;

		// Bytes per token, the same estimate the loop's budget uses.
		inline constexpr std::size_t SCHEMA_BYTES_PER_TOKEN = 4;

		// One exported block is capped at this, cut on a newline so the file
		// stays valid UTF-8. A tool result can be megabytes of JSON.
		inline constexpr std::size_t EXPORT_BLOCK_MAX_BYTES = 4096;

		inline constexpr std::string_view EXPORT_FILE_PREFIX = "mcode-session-";
		inline constexpr std::string_view EXPORT_FILE_SUFFIX = ".md";


		[[nodiscard]] auto capped_block( const std::string_view text ) -> std::string {
			if ( text.size( ) <= EXPORT_BLOCK_MAX_BYTES ) {
				return std::string{ text };
			}

			auto cut = text.rfind( '\n', EXPORT_BLOCK_MAX_BYTES );

			if ( cut == std::string_view::npos ) {
				cut = EXPORT_BLOCK_MAX_BYTES;
			}

			return std::string{ text.substr( 0, cut ) } + "\n(truncated)\n";
		}

		auto append_quoted( std::string& out, const std::string_view text ) -> void {
			auto start = std::size_t{ 0 };

			while ( start <= text.size( ) ) {
				const auto newline = text.find( '\n', start );
				const auto end = newline == std::string_view::npos ? text.size( ) : newline;

				out += "> ";
				out.append( text.substr( start, end - start ) );
				out += '\n';

				if ( newline == std::string_view::npos ) {
					break;
				}

				start = newline + 1;
			}

			out += '\n';
		}

		[[nodiscard]] auto help_text( const std::vector< mcode::tui::slash_command >& commands )
			-> std::string {
			auto out = std::string{ };

			for ( const auto& entry : commands ) {
				out += "/" + entry.name + "  " + entry.description + '\n';
			}

			return out;
		}

		[[nodiscard]] auto context_text( const std::uint64_t used,
			const std::uint64_t capacity ) -> std::string {
			if ( capacity == 0 ) {
				return "context: " + std::to_string( used ) +
					" tokens used; the model's context window is unknown";
			}

			return "context: " + std::to_string( used ) + " / " + std::to_string( capacity ) +
				" tokens (" + std::to_string( used * PERCENT_SCALE / capacity ) + "%)";
		}

		[[nodiscard]] auto compact_text( ) -> std::string {
			const auto trigger = static_cast< std::uint64_t >(
				mcode::COMPACTION_TRIGGER_FRACTION * static_cast< double >( PERCENT_SCALE ) );

			return "compaction is not implemented as a command; the loop compacts the history "
				"itself once the context window is " + std::to_string( trigger ) + "% full";
		}

		// The prompt `/init` submits. The command writes nothing itself: the file
		// is generated from what is actually in the repository, which is the whole
		// difference between this and a scaffold with empty sections.
		//
		// Every rule below is a measured finding, not taste. The sources:
		//
		//  - Size dominates. Anthropic's target is under 200 lines per instruction
		//    file because "longer files consume more context and reduce adherence";
		//    IFScale measured the best frontier models at 68% adherence at 500
		//    instructions, degrading all of them uniformly rather than just the
		//    newest. A study of 100 popular repositories found Context Bloat in 42
		//    and a worst case of 1,477 lines across 27 sections.
		//  - Negative constraints beat positive directives. A 5,000-run study
		//    found every individually beneficial rule was a negative constraint
		//    and every individually harmful one a positive directive.
		//  - Lint Leakage is the most common smell (62% of files): restating what
		//    a formatter already enforces. Skill Leakage is 35%: rare procedures
		//    that belong in an on-demand skill.
		//  - Repository overviews are the one content class measured as *not*
		//    helpful, while adding over 20% inference cost.
		//  - The payoff is efficiency, not correctness: a paired 124-PR study
		//    measured median runtime -28.6% and output tokens -16.6%, with no
		//    significant correctness change.
		//  - Generated files fossilize. 24 of 100 sampled files had never been
		//    edited after generation, which is why the prompt demands evidence and
		//    why `/init` refuses to overwrite an existing file.
		//
		// A raw string with a custom delimiter, because the prompt quotes phrases
		// containing `)"`.
		inline constexpr std::string_view INIT_PROMPT = R"AGENTS(Write an AGENTS.md for this repository.

AGENTS.md is the instruction file a coding agent loads on EVERY request. It is
not documentation and not a README. Its measurable payoff is fewer wasted steps
-- a paired study of 124 pull requests found it cuts median runtime by 28.6% and
output tokens by 16.6%. Its measurable cost is context: every line is paid for
on every request and dilutes the lines around it. Length is the dominant
variable in whether the file works at all.

So this is a subtraction exercise. The hard part is not finding things to say --
it is leaving out what the repository already says.

STEP 1 -- Explore. Do not write anything yet.

Read the files that define how this project actually runs:
- The build manifest: CMakeLists.txt, package.json, Cargo.toml, pyproject.toml,
  Makefile, go.mod, build.gradle, or whatever this repository uses.
- The CI workflow under .github/workflows/ (or the equivalent). CI is where the
  real commands live, with the real flags.
- Any scripts/ or tools/ directory that holds a build, test or release script.
- README, CONTRIBUTING, and docs/. You need to know what is already documented
  so you can point at it instead of repeating it.
- Any existing AGENTS.md, CLAUDE.md, .cursorrules or CONTRIBUTING conventions.

STEP 2 -- Find the traps. This is where the value is.

A trap is something a competent engineer would get wrong without being told, or
would only discover by breaking it. Examples of the shape, not a checklist:
- A file that is generated and must not be edited by hand.
- Two things that must be changed together or the build breaks.
- A test that needs a service, a fixture or a specific order to pass.
- A flag that must never be passed outside local development.
- A directory whose name does not mean what it looks like.

Look for these in the code, in CI, in comments near the fragile thing, and in
git log for reverts and fixes. A trap you cannot point at evidence for is not a
trap -- leave it out.

STEP 3 -- Write AGENTS.md at the repository root with the write tool.

Structure, in this order. Omit any section you cannot fill from evidence:

1. Title and ONE sentence saying what the project is. One sentence, not a
   paragraph and not an overview -- repository overviews are the one content
   class measured as unhelpful, while adding over 20% inference cost.

2. Commands. The most valuable block in the file. Exact invocations WITH their
   flags: how to build, how to run the full test suite, how to run a single
   test, how to lint, how to format. `pytest` is nearly useless; the exact
   invocation the CI runs is what saves a round trip. Copy them from the
   manifest or CI, not from memory.

3. What "done" means. The exact command or exit code that proves a change
   works. An agent that cannot verify reports "looks done" and makes you the
   verification loop.

4. Constraints and traps, phrased as things NOT to do. "Do not edit
   generated/foo.cxx by hand" beats "prefer to modify the source template".
   This is the strongest measured finding in the whole document: negative
   constraints help, positive directives actively hurt. Write "do not X" and
   give the reason when the reason is not obvious.

5. Boundaries, as three short lists: always, ask first, never. Never-commit
   secrets, never force-push, never edit vendored code -- whatever is true here.

6. What to do when blocked. Without this an agent invents workarounds: deleting
   lock files, skipping hooks, ignoring a failing test. One line is enough --
   "if the suite fails twice after a fix, stop and report the failure".

7. Pointers to deeper docs, each with what it contains and when to read it. A
   bare path is ignored or over-loaded; the reason is what makes it work.

LEAVE OUT, and this list matters more than the one above:

- Anything a formatter, linter or type checker already enforces. Indentation,
  line length, naming conventions, import order, docstring rules. These are
  the most common content in real AGENTS.md files and the least useful: a
  deterministic tool checks them, and restating them crowds out what matters.
- Anything derivable from reading the code. If the agent can find it in one
  file, it does not belong here.
- Standard language conventions. "Use const correctness in C++" is not a
  project fact.
- Generic advice: "write tests", "keep functions small", "follow the existing
  style", "be careful". True of every repository, changes nothing here.
- A file-by-file inventory or an architecture essay. Describe capabilities at
  directory granularity, or name the doc and point at it.
- Rare multi-step procedures. If it applies to one task in fifty, it belongs in
  a skill loaded on demand, not in a file loaded always.
- Any section you could not fill from evidence. An omitted section is correct.
  An invented command is worse than no file, because the reader will run it.

SIZE: aim for 100 lines or fewer. Hard ceiling 150. The whole instruction chain
-- this file plus any user or organization file -- has a 2K token budget, so a
generous file here takes the budget from the rest.

MECHANICS: Markdown headings, at most two levels. No placeholders, no TODO, no
empty headings, no commented-out templates. No emoji.

BEFORE YOU WRITE, apply this test to every line you have drafted:

  "If this line were deleted, would the agent make a mistake?"

If the answer is no, delete it. This one test removes most of what a generated
file usually contains.

If the repository is small enough that there is genuinely nothing non-obvious to
say, write the smallest useful file -- the commands and the done definition are
still worth having -- and do not pad it. In your reply, say what you could not
determine rather than guessing, and tell me which lines you were least sure of,
because I have to review this file before it is worth anything.)AGENTS";

		// `/init` refuses when the file exists rather than overwriting: AGENTS.md
		// is the user's, and replacing it would destroy the instructions the agent
		// is meant to follow. Returns the message when it exists, nothing when the
		// generation should proceed.
		[[nodiscard]] auto init_existing_note( const mcode::agent_loop& loop )
			-> std::optional< std::string > {
			auto error_code = std::error_code{ };
			const auto root = loop.workspace_root( );
			const auto directory = root.empty( )
				? std::filesystem::current_path( error_code )
				: std::filesystem::path{ std::string{ root } };

			if ( error_code ) {
				return "cannot write AGENTS.md: the working directory is unreadable";
			}

			const auto target = directory / "AGENTS.md";

			if ( std::filesystem::exists( target, error_code ) && !error_code ) {
				return "AGENTS.md already exists at " + target.string( ) +
					"; delete it first if you want it regenerated";
			}

			return std::nullopt;
		}

		// Reports what is loaded and what it costs, which is what a context budget needs and
		// what a broken config needs. Only the facts this build can actually observe.
		[[nodiscard]] auto doctor_text( const mcode::agent_loop& loop ) -> std::string {
			auto out = std::string{ "mcode doctor\n" };

			out += "  model: " + std::string{ loop.model_name( ) } + '\n';

			const auto root = loop.workspace_root( );

			out += "  workspace: " + ( root.empty( ) ? std::string{ "(unset)" }
													: std::string{ root } ) + '\n';

			auto tools = std::size_t{ 0 };
			auto schema_bytes = std::size_t{ 0 };

			for ( const auto* tool : loop.registry( ).all( ) ) {
				++tools;
				schema_bytes += tool->name.size( ) + tool->description.size( ) +
					tool->schema_json.size( );
			}

			out += "  tools: " + std::to_string( tools ) + " registered, " +
				std::to_string( schema_bytes ) + " bytes of schema (~" +
				std::to_string( schema_bytes / SCHEMA_BYTES_PER_TOKEN ) + " tokens)\n";

			const auto capacity = loop.context_capacity( );

			out += "  context: " + std::to_string( loop.context_used( ) ) + " used";
			out += capacity == 0
				? std::string{ "; the model's context window is unknown\n" }
				: " of " + std::to_string( capacity ) + "\n";

			const auto* store = loop.snapshots( );

			out += std::string{ "  snapshots: " } +
				( store == nullptr ? "not configured; /undo has nothing to restore"
									: "configured; /undo and /rewind are available" ) + '\n';

			const auto instructions = loop.instruction_chain( );

			out += "  instructions: " +
				( instructions.empty( )
					? std::string{ "none loaded; run /init to create an AGENTS.md" }
					: std::to_string( instructions.size( ) ) + " bytes loaded (~" +
						std::to_string( instructions.size( ) / SCHEMA_BYTES_PER_TOKEN ) +
						" tokens)" ) + '\n';

			return out;
		}

		[[nodiscard]] auto export_text( const mcode::agent_loop& loop,
			const std::string_view argument ) -> std::string {
			auto error_code = std::error_code{ };
			const auto root = loop.workspace_root( );
			const auto directory = root.empty( )
				? std::filesystem::current_path( error_code )
				: std::filesystem::path{ std::string{ root } };

			if ( error_code ) {
				return "export failed: cannot read the working directory: " +
					error_code.message( );
			}

			auto path = std::filesystem::path{ };

			if ( argument.empty( ) ) {
				path = directory / ( std::string{ EXPORT_FILE_PREFIX } +
					std::to_string( mcode::support::epoch_milliseconds( ) ) +
					std::string{ EXPORT_FILE_SUFFIX } );
			} else {
				path = directory / std::filesystem::path{ std::string{ argument } };
			}

			auto out = std::ofstream{ path, std::ios::binary | std::ios::trunc };

			if ( !out ) {
				return "export failed: cannot write " + path.string( );
			}

			out << render_transcript_markdown( loop.history( ) );
			out.flush( );

			if ( !out ) {
				return "export failed: writing " + path.string( ) + " did not complete";
			}

			return "exported " + std::to_string( loop.history( ).size( ) ) + " messages to " +
				path.string( );
		}

		// The workspace the session edits in; empty means the process working directory.
		[[nodiscard]] auto undo_root( const mcode::agent_loop& loop ) -> std::filesystem::path {
			const auto root = loop.workspace_root( );

			if ( !root.empty( ) ) {
				return std::filesystem::path{ std::string{ root } };
			}

			auto error_code = std::error_code{ };
			const auto current = std::filesystem::current_path( error_code );

			return error_code ? std::filesystem::path{ } : current;
		}

		// Both commands are the same report; only which captures they replay differs.
		[[nodiscard]] auto restore_report( const mcode::agent_loop& loop, const bool every_run )
			-> std::string {
			auto* store = loop.snapshots( );

			if ( store == nullptr ) {
				return "no snapshot store is configured for this session, so nothing was "
					"captured and there is nothing to restore";
			}

			const auto root = undo_root( loop );

			if ( root.empty( ) ) {
				return "cannot restore: the working directory is unreadable";
			}

			const auto run = loop.run_id( );

			if ( !every_run && run.empty( ) ) {
				return "nothing to undo: no run has captured a file yet";
			}

			auto restored = every_run ? store->restore_all( root ) : store->restore_last( root, run );

			if ( !restored ) {
				return "restore failed: " + restored.error( ).msg;
			}

			const auto scope = every_run
				? std::string{ "every capture" }
				: "run " + std::string{ run };

			return "restored " + std::to_string( *restored ) + " file(s) from " + scope;
		}

		// Columns in `/extensions`: the name column is sized to the longest name,
		// so the version and tool columns line up without a fixed width that a
		// long extension name would break.
		inline constexpr std::size_t EXTENSION_COLUMN_GAP = 2;

		// A name padded to `width` so columns line up. A name already longer than
		// the column keeps its own length rather than being cut.
		[[nodiscard]] auto padded( const std::string_view text, const std::size_t width )
			-> std::string {
			auto out = std::string{ text };

			while ( out.size( ) < width ) {
				out.push_back( ' ' );
			}

			return out;
		}

	}

	// Per-extension memory is a budget the project tracks (`docs/28` measured
	// ~320 KB per VM), and a session's log size is what tells a reader whether it
	// is the one they want. One implementation so the two reports cannot disagree.
	auto human_bytes( const std::uint64_t bytes ) -> std::string {
		constexpr auto BYTES_PER_KILOBYTE = std::uint64_t{ 1024 };

		if ( bytes < BYTES_PER_KILOBYTE ) {
			return std::to_string( bytes ) + " B";
		}

		return std::to_string( bytes / BYTES_PER_KILOBYTE ) + " KB";
	}

	auto extensions_text( const mcode::ext::load_report& report ) -> std::string {
		auto out = std::string{ "extensions: " + std::to_string( report.loaded.size( ) ) +
			" loaded" };

		// Only mentioned when non-zero: a zero is noise on the common path, and
		// the failure lines below are what a reader is looking for.
		if ( !report.failed.empty( ) ) {
			out += ", " + std::to_string( report.failed.size( ) ) + " failed";
		}

		if ( report.disabled > 0 ) {
			out += ", " + std::to_string( report.disabled ) + " disabled";
		}

		out += '\n';

		if ( report.loaded.empty( ) ) {
			out += "\n  none loaded\n";
		}

		auto name_width = std::size_t{ 0 };

		for ( const auto& entry : report.loaded ) {
			name_width = std::max( name_width, entry.name.size( ) );
		}

		for ( const auto& entry : report.loaded ) {
			out += "\n  " + padded( entry.name, name_width ) +
				std::string( EXTENSION_COLUMN_GAP, ' ' );

			if ( !entry.version.empty( ) ) {
				out += entry.version + std::string( EXTENSION_COLUMN_GAP, ' ' );
			}

			if ( entry.tools.empty( ) ) {
				out += "no tools";
			} else {
				out += std::to_string( entry.tools.size( ) ) +
					( entry.tools.size( ) == 1 ? " tool: " : " tools: " );

				for ( auto index = std::size_t{ 0 }; index < entry.tools.size( ); ++index ) {
					if ( index > 0 ) {
						out += ", ";
					}

					out += entry.tools[ index ];
				}
			}

			out += "  " + human_bytes( entry.bytes_used ) + '\n';

			if ( !entry.description.empty( ) ) {
				out += "    " + entry.description + '\n';
			}
		}

		// A failure is the reason someone runs this command, so it is printed in
		// full rather than summarised: the loader's reason names the key or the
		// permission that was refused.
		for ( const auto& failure : report.failed ) {
			out += "\n  " + failure.name + " FAILED: " + failure.reason + '\n';
		}

		return out;
	}

	auto render_transcript_markdown( const std::vector< mcode::model::message >& history )
		-> std::string {
		auto out = std::string{ "# mcode session transcript\n\n" };

		for ( const auto& message : history ) {
			out += "## ";
			out += mcode::model::to_string( message.speaker );
			out += "\n\n";

			for ( const auto& block : message.blocks ) {
				switch ( block.kind ) {
					case mcode::model::block_kind::text: {
						out += block.text;
						out += "\n\n";

						break;
					}

					case mcode::model::block_kind::thinking: {
						append_quoted( out, capped_block( block.text ) );

						break;
					}

					case mcode::model::block_kind::tool_call: {
						out += "**" + block.tool_name + "**\n\n```json\n";
						out += capped_block( block.args_json );
						out += "```\n\n";

						break;
					}

					case mcode::model::block_kind::tool_result: {
						out += "```\n";
						out += capped_block( block.result_json );
						out += "```\n\n";

						break;
					}
				}
			}
		}

		return out;
	}

auto run_command( const command_match& match, const agent_loop& loop,
		const std::vector< mcode::tui::slash_command >& commands,
		const mcode::ext::load_report* extensions ) -> command_result {
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
		} else if ( match.name == "context" ) {
			result.output = context_text( loop.context_used( ), loop.context_capacity( ) );
		} else if ( match.name == "model" ) {
			result.output = std::string{ loop.model_name( ) };
		} else if ( match.name == "tools" ) {
			for ( const auto* tool : loop.registry( ).all( ) ) {
				if ( !result.output.empty( ) ) {
					result.output += ", ";
				}

				result.output += tool->name;
			}
		} else if ( match.name == "compact" ) {
			result.output = compact_text( );
		} else if ( match.name == "export" ) {
			result.output = export_text( loop, match.arguments );
		} else if ( match.name == "undo" ) {
			result.output = restore_report( loop, false );
		} else if ( match.name == "rewind" ) {
			result.output = restore_report( loop, true );
		} else if ( match.name == "init" ) {
			const auto existing = init_existing_note( loop );

			if ( existing ) {
				result.output = *existing;
			} else {
				// The command itself writes nothing: the model explores the
				// workspace and writes the file, because the point of this over a
				// scaffold is that the content comes from what is actually there.
				result.submit_prompt = std::string{ INIT_PROMPT };
			}
		} else if ( match.name == "extensions" ) {
			result.output = extensions == nullptr
				? std::string{ "extensions: no session to read; none loaded in this process" }
				: extensions_text( *extensions );
		} else if ( match.name == "resume" ) {
			if ( match.arguments.empty( ) ) {
				// No id names the picker rather than the newest session: choosing
				// which one to reopen is the whole point, and guessing would
				// silently discard the session the user is in.
				result.open_session_picker = true;
			} else {
				result.adopt_session_id = std::string{ match.arguments };
			}
		} else if ( match.name == "continue" ) {
			// An empty id means "the newest for this workspace", which is what the
			// REPL resolves -- it has the workspace root, this does not.
			result.adopt_session_id = std::string{ };
		} else if ( match.name == "new" ) {
			result.start_new_session = true;
		} else if ( match.name == "doctor" ) {
			result.output = doctor_text( loop );
		} else if ( match.name == "mention" ) {
			result.open_mention = true;
		} else {
			result.output = help_text( commands );
		}

		return result;
	}

}
