#include "mcode/cli/slash.hxx"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "exec_internal.hxx"

namespace mcode::cli {

	namespace {

		inline constexpr std::string_view COMMAND_PREFIX = "/";

		// One comparison, so the three history tiers cannot drift apart on "case-insensitive".
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

	}

	auto builtin_commands( ) -> const std::vector< mcode::tui::slash_command >& {
		static const auto COMMANDS = std::vector< mcode::tui::slash_command >{
			{ "help", "List the available commands" },
			{ "cost", "Show tokens spent and spend so far" },
			{ "context", "Show context-window usage: used, capacity and percent" },
			{ "model", "Show the model this session runs" },
			{ "tools", "List the tools the agent can call" },
			{ "compact", "Report how the history is compacted (no on-demand compaction)" },
			{ "remember", "Append a note to this project's ledger, verbatim" },
			{ "export", "Write the session transcript to a Markdown file, named by an argument "
				"or generated" },
			{ "undo", "Restore the files the current run changed and report the count" },
			{ "rewind", "Restore every captured file and report the count" },
			{ "init", "Generate an AGENTS.md for this repository, from what is actually in it" },
			{ "extensions", "Show the loaded extensions, their tools and their memory" },
			{ "new", "Start a fresh session, keeping the current one on disk" },
			{ "continue", "Reopen the newest session for this workspace" },
			{ "resume", "Reopen a session: /resume picks one, /resume <id> names it" },
			{ "doctor", "Report what is loaded: model, tools, context, instructions" },
			{ "mention", "Pick a workspace file to mention as @path" },
			{ "prompt", "List an MCP server's prompts, or fill one: /prompt <name> [json args]" },
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

		// Newest first, so the result keeps the order the search walks.
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

	[[nodiscard]] auto session_query( const std::string_view input ) -> std::string_view {
		auto query = input;

		if ( query.starts_with( COMMAND_PREFIX ) ) {
			query.remove_prefix( COMMAND_PREFIX.size( ) );
		}

		constexpr auto COMMAND_WORD = std::string_view{ "resume" };

		if ( query.starts_with( COMMAND_WORD ) ) {
			query.remove_prefix( COMMAND_WORD.size( ) );
		}

		while ( !query.empty( ) && query.front( ) == ' ' ) {
			query.remove_prefix( 1 );
		}

		return query;
	}

	auto inserted_line( const palette_source source, const std::string_view input,
		const mcode::tui::slash_command& row ) -> std::string {
		switch ( source ) {
			case palette_source::mentions:
				return mcode::tui::mention_completion( input, row.name );
			case palette_source::history:
				return row.name;
			case palette_source::sessions:
				return std::string{ "/" } + row.name;
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

			// The mention closed, so the same keystroke is re-read as a command query.
			if ( !palette.open ) {
				source_ = palette_source::commands;
			}
		}

		if ( source_ == palette_source::history ) {
			refresh_history_palette( palette, *sources_.history, input );
		} else if ( source_ == palette_source::sessions && sources_.sessions != nullptr ) {
			refresh_sessions_palette( palette, *sources_.sessions, session_query( input ) );
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

	auto session_rows( const std::vector< session_ref >& sessions )
		-> std::vector< mcode::tui::slash_command > {
		auto rows = std::vector< mcode::tui::slash_command >{ };

		for ( const auto& session : sessions ) {
			auto description = std::string{ };

			if ( session.started_ms > 0 ) {
				description += detail::format_utc( session.started_ms );
				description += "  ";
			}

			description += mcode::text::human_bytes( session.size_bytes );

			// The opening request identifies a session to a person; an id and a timestamp do not.
			if ( auto opening = mcode::cli::session_opening_line( session.path );
				!opening.empty( ) ) {
				description += "  ";
				description += opening;
			} else {
				description += "  (no recorded prompt)";
			}

			rows.push_back( mcode::tui::slash_command{
				"resume " + session.id, std::move( description ) } );
		}

		return rows;
	}

	auto refresh_sessions_palette( mcode::tui::slash_palette& palette,
		const std::vector< mcode::tui::slash_command >& rows, const std::string_view query ) -> void {
		palette.open = true;
		palette.query = std::string{ query };
		palette.prefix.clear( );
		palette.matches.clear( );

		// A substring over the whole row, not the ranked match the command palette uses.
		auto folded_query = std::string{ query };
		std::transform( folded_query.begin( ), folded_query.end( ), folded_query.begin( ),
			[]( const unsigned char character ) {
				return static_cast< char >( std::tolower( character ) );
			} );

		for ( const auto& row : rows ) {
			if ( folded_query.empty( ) ) {
				palette.matches.push_back( row );

				continue;
			}

			auto haystack = row.name + " " + row.description;
			std::transform( haystack.begin( ), haystack.end( ), haystack.begin( ),
				[]( const unsigned char character ) {
					return static_cast< char >( std::tolower( character ) );
				} );

			if ( haystack.find( folded_query ) != std::string::npos ) {
				palette.matches.push_back( row );
			}
		}

		if ( palette.selected >= palette.matches.size( ) ) {
			palette.selected = 0;
		}
	}

	auto palette_controller::open_sessions( mcode::tui::slash_palette& palette,
		const std::string_view query ) -> palette_source {
		source_ = palette_source::sessions;

		// No source wired stays on the commands rather than opening an empty list.
		if ( sources_.sessions == nullptr ) {
			source_ = palette_source::commands;

			refresh_palette( palette, *sources_.commands, query );

			return source_;
		}

		refresh_sessions_palette( palette, *sources_.sessions, session_query( query ) );

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

}
