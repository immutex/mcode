#include "mcode/cli/slash.hxx"

#include "mcode/cli/init_prompt.hxx"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "mcode/fs/snapshot.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/support/text.hxx"
#include "mcode/support/time.hxx"

namespace mcode::cli {

	namespace {


		inline constexpr std::uint64_t PERCENT_SCALE = 100;

		// Bytes per token: the shared estimate, not a fourth copy of it.
		inline constexpr std::size_t SCHEMA_BYTES_PER_TOKEN = mcode::text::CHARS_PER_TOKEN;

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

		// See `init_prompt.hxx`: the prompt is data, and it is long.

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

				// `operator/` replaces the base when the right side is absolute, and
				// `..` walks out, so `/export /etc/passwd` or `/export ../../x` wrote
				// outside the workspace. Every other write in this program goes
				// through the workspace boundary; this one has to as well. Compared
				// canonically, so `notes/../out.md` is allowed and `../out.md` is not.
				auto canonical_error = std::error_code{ };
				const auto canonical_root = std::filesystem::weakly_canonical(
					directory, canonical_error );
				const auto canonical_path = std::filesystem::weakly_canonical(
					path, canonical_error );

				if ( canonical_error || !mcode::path_is_within( canonical_root, canonical_path ) ) {
					return "export failed: '" + std::string{ argument } +
						"' is outside the workspace; give a path inside " +
						directory.string( );
				}

				path = canonical_path;
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

			out += "  " + mcode::text::human_bytes( entry.bytes_used ) + '\n';

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
				result.submit_prompt = std::string{ detail::INIT_PROMPT };
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
