#include "mcode/cli/slash.hxx"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "mcode/fs/snapshot.hxx"
#include "mcode/support/time.hxx"

namespace mcode::cli {

	namespace {

		// The mention picker's rows are paths, so they carry no prefix.
		inline constexpr std::string_view COMMAND_PREFIX = "/";

		inline constexpr std::uint64_t PERCENT_SCALE = 100;

		// Bytes per token, the same estimate the loop's budget uses.
		inline constexpr std::size_t SCHEMA_BYTES_PER_TOKEN = 4;

		// One exported block is capped at this, cut on a newline so the file
		// stays valid UTF-8. A tool result can be megabytes of JSON.
		inline constexpr std::size_t EXPORT_BLOCK_MAX_BYTES = 4096;

		inline constexpr std::string_view EXPORT_FILE_PREFIX = "mcode-session-";
		inline constexpr std::string_view EXPORT_FILE_SUFFIX = ".md";

		// One comparison, so the three history tiers cannot drift apart on
		// what "case-insensitive" means.
		[[nodiscard]] auto folded_equal( const char left, const char right ) -> bool {
			const auto fold = []( const char value ) {
				return std::tolower( static_cast< unsigned char >( value ) );
			};

			return fold( left ) == fold( right );
		}

		[[nodiscard]] auto folded_find( const std::string_view text,
			const std::string_view needle ) -> std::size_t {
			if ( needle.empty( ) ) {
				return 0;
			}

			if ( needle.size( ) > text.size( ) ) {
				return std::string_view::npos;
			}

			for ( auto start = std::size_t{ 0 }; start + needle.size( ) <= text.size( ); ++start ) {
				if ( std::equal( needle.begin( ), needle.end( ), text.begin( ) +
						static_cast< std::ptrdiff_t >( start ), folded_equal ) ) {
					return start;
				}
			}

			return std::string_view::npos;
		}

		[[nodiscard]] auto folded_subsequence( const std::string_view text,
			const std::string_view query ) -> bool {
			auto index = std::size_t{ 0 };

			for ( const auto character : text ) {
				if ( index < query.size( ) && folded_equal( character, query[ index ] ) ) {
					++index;
				}
			}

			return index == query.size( );
		}

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

		// The scaffold is deliberately short: an instruction file that repeats what the
		// repository already says costs context on every request and is ignored.
		inline constexpr std::string_view INIT_SCAFFOLD =
			"# AGENTS.md\n"
			"\n"
			"Instructions for a coding agent working in this repository.\n"
			"\n"
			"## Layout\n"
			"\n"
			"<!-- Where the code lives, and which directories are generated. -->\n"
			"\n"
			"## Commands\n"
			"\n"
			"<!-- Build, test, lint. Exact invocations, not descriptions. -->\n"
			"\n"
			"## Conventions\n"
			"\n"
			"<!-- Only what a reader would otherwise get wrong: naming, formatting, layout. -->\n"
			"\n"
			"## Constraints\n"
			"\n"
			"<!-- Non-obvious rules with the reason they exist. -->\n";

		[[nodiscard]] auto init_text( const mcode::agent_loop& loop ) -> std::string {
			auto error_code = std::error_code{ };
			const auto root = loop.workspace_root( );
			const auto directory = root.empty( )
				? std::filesystem::current_path( error_code )
				: std::filesystem::path{ std::string{ root } };

			if ( error_code ) {
				return "cannot write AGENTS.md: the working directory is unreadable";
			}

			const auto target = directory / "AGENTS.md";

			// Never clobber: the file is the user's, and overwriting it destroys the very
			// instructions the agent is meant to follow.
			if ( std::filesystem::exists( target, error_code ) && !error_code ) {
				return "AGENTS.md already exists at " + target.string( ) + "; leaving it alone";
			}

			auto out = std::ofstream{ target, std::ios::binary };

			if ( !out ) {
				return "cannot write " + target.string( ) +
					": the file could not be opened for writing";
			}

			out << INIT_SCAFFOLD;

			if ( !out ) {
				return "cannot write " + target.string( ) + ": the write failed";
			}

			return "wrote a scaffold to " + target.string( ) +
				"; fill in the sections and mcode will load it as instructions";
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

	}

	auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >& {
		static const auto COMMANDS = std::vector< mcode::tui::slash_command >{
			{ "help", "List the available commands" },
			{ "cost", "Show tokens spent and spend so far" },
			{ "context", "Show context-window usage: used, capacity and percent" },
			{ "model", "Show the model this session runs" },
			{ "tools", "List the tools the agent can call" },
			{ "compact", "Report how the history is compacted (no on-demand compaction)" },
			{ "export", "Write the session transcript to a Markdown file, named by an argument "
				"or generated" },
			{ "undo", "Restore the files the current run changed and report the count" },
			{ "rewind", "Restore every captured file and report the count" },
			{ "init", "Write an AGENTS.md scaffold in the workspace if none exists" },
			{ "doctor", "Report what is loaded: model, tools, context, instructions" },
			{ "mention", "Pick a workspace file to mention as @path" },
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
		palette.prefix = std::string{ COMMAND_PREFIX };
		palette.matches = mcode::tui::filter_commands( commands, *query );

		if ( palette.selected >= palette.matches.size( ) ) {
			palette.selected = 0;
		}
	}

	auto refresh_mention_palette( mcode::tui::slash_palette& palette,
		const mcode::tui::mention_index& files, const std::string_view input ) -> void {
		const auto query = mcode::tui::mention_query( input );

		if ( !query ) {
			palette.open = false;
			palette.matches.clear( );
			palette.selected = 0;

			return;
		}

		palette.open = true;
		palette.query = std::string{ *query };
		palette.prefix.clear( );
		palette.matches = files.matches( *query );

		if ( palette.selected >= palette.matches.size( ) ) {
			palette.selected = 0;
		}
	}

	auto rank_history( const std::vector< std::string >& entries, const std::string_view query,
		const std::size_t limit ) -> std::vector< std::string > {
		auto out = std::vector< std::string >{ };

		if ( limit == 0 || entries.empty( ) ) {
			return out;
		}

		// Newest first: the entry the user most likely wants is the one they
		// just submitted, so the search runs backwards and the result keeps
		// that order.
		const auto collect = [ & ]( const auto& accepted ) {
			for ( auto index = entries.size( ); index > 0; --index ) {
				if ( out.size( ) >= limit ) {
					return;
				}

				const auto& entry = entries[ index - 1 ];

				if ( accepted( entry ) ) {
					out.push_back( entry );
				}
			}
		};

		if ( query.empty( ) ) {
			collect( []( const std::string& ) { return true; } );

			return out;
		}

		const auto folded_prefix = [ & ]( const std::string& entry ) {
			return entry.size( ) >= query.size( ) &&
				std::equal( query.begin( ), query.end( ), entry.begin( ), folded_equal );
		};
		const auto contains = [ & ]( const std::string& entry ) {
			return folded_find( entry, query ) != std::string::npos;
		};

		collect( folded_prefix );
		collect( [ & ]( const std::string& entry ) {
			return !folded_prefix( entry ) && contains( entry );
		} );
		collect( [ & ]( const std::string& entry ) {
			return !folded_prefix( entry ) && !contains( entry ) &&
				folded_subsequence( entry, query );
		} );

		return out;
	}

	auto refresh_history_palette( mcode::tui::slash_palette& palette,
		const std::vector< std::string >& entries, const std::string_view query ) -> void {
		palette.open = true;
		palette.query = std::string{ query };
		palette.prefix.clear( );
		palette.matches.clear( );

		for ( const auto& entry : rank_history( entries, query, HISTORY_LIMIT ) ) {
			palette.matches.push_back( mcode::tui::slash_command{ entry, std::string{ } } );
		}

		if ( palette.selected >= palette.matches.size( ) ) {
			palette.selected = 0;
		}
	}

	auto inserted_line( const palette_source source, const std::string_view input,
		const mcode::tui::slash_command& row ) -> std::string {
		switch ( source ) {
			case palette_source::mentions:
				return mcode::tui::mention_completion( input, row.name );
			case palette_source::history:
				return row.name;
			case palette_source::commands:
				break;
		}

		return mcode::tui::completed_command( row.name );
	}

	auto palette_controller::refresh( mcode::tui::slash_palette& palette,
		const std::string_view input ) -> palette_source {
		if ( source_ != palette_source::history && sources_.files != nullptr &&
			mcode::tui::mention_query( input ).has_value( ) ) {
			source_ = palette_source::mentions;
		}

		if ( source_ == palette_source::mentions ) {
			refresh_mention_palette( palette, *sources_.files, input );

			// The mention closed, so the same keystroke is re-read as a
			// command query rather than costing a second keystroke.
			if ( !palette.open ) {
				source_ = palette_source::commands;
			}
		}

		if ( source_ == palette_source::history ) {
			refresh_history_palette( palette, *sources_.history, input );
		} else if ( source_ == palette_source::commands && sources_.commands != nullptr ) {
			refresh_palette( palette, *sources_.commands, input );
		}

		return source_;
	}

	auto palette_controller::open_history( mcode::tui::slash_palette& palette,
		const std::string_view query ) -> palette_source {
		source_ = palette_source::history;

		refresh_history_palette( palette, *sources_.history, query );

		return source_;
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
			result.output = init_text( loop );
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
