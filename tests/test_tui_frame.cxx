#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/transcript.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t ROWS = 4;
	constexpr std::size_t COLUMNS = 40;

	[[nodiscard]] auto row_width( const styled_line& row ) -> std::size_t {
		auto total = std::size_t{ 0 };

		for ( const auto& span : row ) {
			total += string_width( span.text, 1 );
		}

		return total;
	}

	[[nodiscard]] auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	[[nodiscard]] auto row_bold( const styled_line& row ) -> bool {
		for ( const auto& span : row ) {
			if ( span.bold ) {
				return true;
			}
		}

		return false;
	}

	// A state's rendered rows against a fresh render of the same buffer: a
	// cache that lags shows up as a text difference, which is what this
	// compares.
	[[nodiscard]] auto same_rows( const std::vector< styled_line >& left,
		const std::vector< styled_line >& right ) -> bool {
		if ( left.size( ) != right.size( ) ) {
			return false;
		}

		for ( auto index = std::size_t{ 0 }; index < left.size( ); ++index ) {
			if ( row_text( left[ index ] ) != row_text( right[ index ] ) ) {
				return false;
			}
		}

		return true;
	}

}

TEST_CASE( "the width function truncates on cluster boundaries", "[tui][width]" ) {
	const auto ambiguous = std::size_t{ 1 };

	const auto cjk = std::string{ "\xE4\xB8\x96\xE7\x95\x8C" };
	REQUIRE( string_width( cjk, ambiguous ) == 4 );

	// a truncation never splits a grapheme cluster.
	const auto cut = truncate_to_width( cjk, 3, ambiguous );
	CHECK( cut == std::string{ "\xE4\xB8\x96" } );

	const auto combining = std::string{ "e\xCC\x81x" };
	CHECK( string_width( combining, ambiguous ) == 2 );

	const auto mark_cut = truncate_to_width( combining, 1, ambiguous );
	CHECK( mark_cut == std::string{ "e\xCC\x81" } );

	// a truncation that does not fit drops the whole ZWJ sequence.
	const auto family = std::string{ "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
		"\xE2\x80\x8D\xF0\x9F\x91\xA7" };
	CHECK( string_width( family, ambiguous ) == 2 );
	CHECK( truncate_to_width( family, 1, ambiguous ).empty( ) );
	CHECK( truncate_to_width( family, 2, ambiguous ) == family );

	CHECK( string_width( "hello", ambiguous ) == 5 );
	CHECK( truncate_to_width( "hello", 3, ambiguous ) == "hel" );
}

TEST_CASE( "a frame diff emits nothing when the state does not change", "[tui][frame]" ) {
	auto state = render_state{ };
	state.model_name = "test-model";
	state.total_tokens = 1200;
	state.total_cost = 0.031;
	state.turn_elapsed_ms = 4200;
	state.input_line = "fix the test";

	const auto first = build_frame( state, ROWS, COLUMNS, 1 );
	const auto second = build_frame( state, ROWS, COLUMNS, 1 );

	REQUIRE( first.rows( ) == second.rows( ) );

	const auto runs = diff_rows( first, second );
	CHECK( runs.empty( ) );
}

TEST_CASE( "a frame diff emits only the changed cells when one line changes",
	"[tui][frame]" ) {
	auto before = render_state{ };
	before.model_name = "m";
	before.input_line = "abc";

	auto after = before;
	after.input_line = "abx";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	const auto runs = diff_rows( previous, current );

	REQUIRE( runs.size( ) == 1 );
	CHECK( runs.front( ).row == ROWS - 1 );
	CHECK( runs.front( ).column == 4 );
	CHECK( runs.front( ).count == 1 );
}

TEST_CASE( "rendering a fixed state twice produces identical bytes", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::truecolor;

	auto state = render_state{ };
	state.model_name = "m";
	state.total_tokens = 500;
	state.total_cost = 0.5;
	state.turn_elapsed_ms = 1000;
	state.input_line = "hello";

	auto tool = render_state::active_tool{ };
	tool.verb = "read";
	tool.target = "src/main.cxx";
	tool.elapsed_ms = 1200;
	tool.spinner_frame = 3;
	state.tools.push_back( tool );

	const auto previous = build_frame( state, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto first = emitter.emit( previous, previous );

	auto emitter_again = ansi_emitter{ caps };
	const auto second = emitter_again.emit( previous, previous );

	CHECK( first == second );
	CHECK( first.empty( ) );
}

TEST_CASE( "the emitter writes only changed runs with SGR deltas", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::ansi16;

	auto before = render_state{ };
	before.input_line = "abcdef";

	auto after = before;
	after.input_line = "abcxef";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto bytes = emitter.emit( previous, current );

	CHECK( bytes.find( "x" ) != std::string::npos );
	CHECK( bytes.find( "abc" ) == std::string::npos );
	CHECK( bytes.find( "def" ) == std::string::npos );
}

TEST_CASE( "NO_COLOR produces attributes-only output", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto before = render_state{ };
	before.input_line = "abc";

	auto after = before;
	after.input_line = "abd";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto bytes = emitter.emit( previous, current );

	CHECK( bytes.find( "38;" ) == std::string::npos );
	CHECK( bytes.find( "48;" ) == std::string::npos );
	CHECK( bytes.find( "d" ) != std::string::npos );
}

TEST_CASE( "the theme resolves one depth per session", "[tui][theme]" ) {
	CHECK( token_color( token::accent, capabilities::color_depth::truecolor ) ==
		"38;2;122;162;247" );
	CHECK( token_color( token::accent, capabilities::color_depth::ansi256 ) == "38;5;111" );
	CHECK( token_color( token::accent, capabilities::color_depth::ansi16 ) == "94" );
	CHECK( token_color( token::accent, capabilities::color_depth::none ).empty( ) );

	// `none` is the absent sentinel and must resolve to no colour at all; a
	// value here would be emitted after the real one and win.
	CHECK( token_color( token::none, capabilities::color_depth::truecolor ).empty( ) );
	CHECK( token_color( token::none, capabilities::color_depth::ansi16 ).empty( ) );

	// A background token is stored foreground-shaped, so it must be rewritten
	// into the 48 family; reading it through token_color would emit a second
	// foreground and the code block would render as near-black text.
	CHECK( token_background( token::code_bg, capabilities::color_depth::truecolor ) ==
		"48;2;26;27;38" );
	CHECK( token_background( token::code_bg, capabilities::color_depth::ansi256 ) ==
		"48;5;234" );
	CHECK( token_background( token::none, capabilities::color_depth::truecolor ).empty( ) );

	for ( const auto& entry : theme_table( ) ) {
		if ( entry.name == token::none ) {
			continue;
		}

		CHECK_FALSE( token_color( entry.name, capabilities::color_depth::truecolor ).empty( ) );
		CHECK_FALSE( std::string_view{ entry.truecolor }.empty( ) );
	}
}

TEST_CASE( "the capability probe honours NO_COLOR and the depth ladder", "[tui][tty]" ) {
	const auto plain = probe_capabilities( "", "", false, true );
	CHECK( plain.depth == capabilities::color_depth::ansi16 );

	const auto truecolor = probe_capabilities( "truecolor", "xterm-256color", false, true );
	CHECK( truecolor.depth == capabilities::color_depth::truecolor );

	const auto indexed = probe_capabilities( "", "xterm-256color", false, true );
	CHECK( indexed.depth == capabilities::color_depth::ansi256 );

	const auto no_color = probe_capabilities( "truecolor", "xterm-256color", true, true );
	CHECK( no_color.depth == capabilities::color_depth::none );

	const auto piped = probe_capabilities( "truecolor", "xterm-256color", false, false );
	CHECK( piped.depth == capabilities::color_depth::truecolor );
	CHECK_FALSE( piped.synchronized_output );
}

TEST_CASE( "synchronized output wraps only when supported and non-empty",
	"[tui][frame]" ) {
	auto supported = capabilities{ };
	supported.synchronized_output = true;

	auto emitter = ansi_emitter{ supported };
	CHECK( emitter.synchronized( "x" ) == "\x1b[?2026hx\x1b[?2026l" );
	CHECK( emitter.synchronized( { } ).empty( ) );

	auto unsupported = capabilities{ };
	unsupported.synchronized_output = false;

	auto plain = ansi_emitter{ unsupported };
	CHECK( plain.synchronized( "x" ) == "x" );
}

TEST_CASE( "spinner glyphs are single-width and cycle", "[tui][render]" ) {
	for ( auto frame = std::size_t{ 0 }; frame < SPINNER_FRAMES * 2; ++frame ) {
		const auto glyph = spinner_glyph( frame );
		CHECK( string_width( glyph, 1 ) == 1 );
	}

	CHECK( spinner_glyph( 0 ) == spinner_glyph( SPINNER_FRAMES ) );
}

TEST_CASE( "the first frame is drawn, not swallowed", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "fix the test", 12 );

	const auto bytes = coordinator.flush( );

	CHECK_FALSE( bytes.empty( ) );
	CHECK( bytes.find( "fix the test" ) != std::string::npos );
	CHECK( bytes.find( "> " ) != std::string::npos );
}

TEST_CASE( "an erased cell reaches the terminal as a space", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	coordinator.set_prompt( "hello", 5 );
	const auto typed = coordinator.flush( );
	REQUIRE( typed.find( "hello" ) != std::string::npos );

	coordinator.set_prompt( "hell", 4 );
	const auto erased = coordinator.flush( );

	CHECK_FALSE( erased.empty( ) );

	// the removed column must be repainted, or the old glyph stays on screen.
	auto visible = std::string{ };

	for ( std::size_t index = 0; index < erased.size( ); ++index ) {
		if ( erased[ index ] == '\x1b' ) {
			while ( index < erased.size( ) &&
				!std::isalpha( static_cast< unsigned char >( erased[ index ] ) ) ) {
				++index;
			}

			continue;
		}

		visible.push_back( erased[ index ] );
	}

	CHECK( visible == " " );
}

TEST_CASE( "the caret lands on the prompt row, under the typed text",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "hi", 2 );

	const auto bytes = coordinator.flush( );

	// the caret column is 1-based CHA.
	const auto expected = std::string{ "\x1b[5G" };

	CHECK( bytes.ends_with( expected ) );
}

TEST_CASE( "the caret column is measured in display cells, not bytes",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	// two bytes, one column: a byte-count caret would sit one cell too far right.
	coordinator.set_prompt( "\u00e9", 2 );

	CHECK( coordinator.state( ).input_cursor == 1 );
}

TEST_CASE( "every escape the emitter writes is well formed", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::ansi16;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "hello", 5 );

	const auto bytes = coordinator.flush( );
	REQUIRE_FALSE( bytes.empty( ) );

	auto cursor = std::size_t{ 0 };
	auto sequences = std::size_t{ 0 };

	while ( cursor < bytes.size( ) ) {
		if ( bytes[ cursor ] != '\x1b' ) {
			CHECK( bytes[ cursor ] != ';' );
			++cursor;

			continue;
		}

		++sequences;
		REQUIRE( cursor + 1 < bytes.size( ) );
		REQUIRE( bytes[ cursor + 1 ] == '[' );

		cursor += 2;

		// only digits, ';' and '?' may appear before the final letter byte.
		while ( cursor < bytes.size( ) && !std::isalpha(
			static_cast< unsigned char >( bytes[ cursor ] ) ) ) {
			const auto character = bytes[ cursor ];
			const auto is_parameter = ( character >= '0' && character <= '9' ) ||
				character == ';' || character == '?';

			REQUIRE( is_parameter );

			++cursor;
		}

		REQUIRE( cursor < bytes.size( ) );
		++cursor;
	}

	CHECK( sequences > 0 );
}

TEST_CASE( "an active tool row names the tool and runs its clock", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "bash";
	start.stamp_ms = 1'000;
	coordinator.apply( start );

	coordinator.advance_tools( 5'000 );

	CHECK( coordinator.state( ).tools.size( ) == 1 );
	CHECK( coordinator.state( ).tools.front( ).verb == "bash" );
	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 );

	const auto first_frame = coordinator.state( ).tools.front( ).spinner_frame;

	coordinator.advance_tools( 5'000 + SPINNER_INTERVAL_MS * 3 );

	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 + SPINNER_INTERVAL_MS * 3 );
	CHECK( coordinator.state( ).tools.front( ).spinner_frame != first_frame );

	// a stamp of zero means no start time was supplied, so the row keeps its initial value.
	auto unstamped = render_coordinator{ };
	unstamped.set_capabilities( caps );
	unstamped.resize( 24, 40 );

	auto unknown = event_queue::item{ };
	unknown.type = event_queue::kind::tool_start;
	unknown.text = "read";
	unstamped.apply( unknown );
	unstamped.advance_tools( 9'999'999 );

	CHECK( unstamped.state( ).tools.front( ).elapsed_ms == 0 );
}

TEST_CASE( "a finished tool call is written to scrollback", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "read";
	coordinator.apply( start );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::tool_end;
	end.text = "read src/main.cxx";
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "read src/main.cxx" ) != std::string::npos );
	CHECK( bytes.find( '\n' ) != std::string::npos );
}

TEST_CASE( "a committed answer is erased from the region and printed",
	"[tui][render]" ) {
	// the region must be erased before printing, or the next frame repaints over the text.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 100 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "Red";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "\x1b[0J" ) != std::string::npos );
	CHECK( bytes.find( "Red" ) != std::string::npos );
}

TEST_CASE( "a chain of thought commits as a block, not one line", "[tui][render]" ) {
	// the collapsed block shows the reasoning, bounded: the first line alone
	// hid everything the model actually thought.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = "first thought\nsecond thought\nthird thought";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first thought" ) != std::string::npos );
	CHECK( bytes.find( "second thought" ) != std::string::npos );
	CHECK( bytes.find( "third thought" ) != std::string::npos );
}

TEST_CASE( "a chain of thought longer than its budget commits its newest rows",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < THOUGHT_COMMIT_MAX_ROWS + 2; ++index ) {
		text += "thought " + std::to_string( index ) + "\n";
	}

	text += "last thought";

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = text;
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	// one committed row per rendered row, bounded to the budget.
	REQUIRE( coordinator.state( ).pending_commit.size( ) == THOUGHT_COMMIT_MAX_ROWS );

	const auto bytes = coordinator.flush( );

	// the block keeps its newest rows, so the head of the chain is dropped.
	CHECK( bytes.find( "✻ thought 3" ) != std::string::npos );
	CHECK( bytes.find( "last thought" ) != std::string::npos );
	CHECK( bytes.find( "thought 0" ) == std::string::npos );
}

TEST_CASE( "a turn's answer survives the turn ending", "[tui][render]" ) {
	// turn_end clears the live region, so the answer must be committed before it.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "the answer";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "the answer" ) != std::string::npos );
}

TEST_CASE( "a streamed answer shows every line, formatted, while it streams",
	"[tui][render]" ) {
	// only the tail of the buffer used to reach the frame, so every line but
	// the last stayed invisible until the turn ended.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "first line\nsecond **line**\nthird line";
	coordinator.apply( delta );

	REQUIRE( coordinator.state( ).streaming_rows.size( ) == 3 );

	// the region grows with the block: prompt + status + three rows.
	CHECK( region_rows_for( coordinator.state( ), 30 ) == 5 );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first line" ) != std::string::npos );
	CHECK( bytes.find( "third line" ) != std::string::npos );

	// the bold span carries its own SGR, so the markers never reach the frame.
	CHECK( bytes.find( "second " ) != std::string::npos );
	CHECK( bytes.find( "line" ) != std::string::npos );
	CHECK( bytes.find( "**" ) == std::string::npos );
}

TEST_CASE( "a streamed block taller than the region keeps its newest rows",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index <= 40; ++index ) {
		text += "row" + std::to_string( index ) + "\n";
	}

	text += "row41";

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = text;
	coordinator.apply( delta );

	REQUIRE( coordinator.state( ).streaming_rows.size( ) == 42 );
	CHECK( region_rows_for( coordinator.state( ), 30 ) == LIVE_REGION_MAX_ROWS );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "row41" ) != std::string::npos );
	CHECK( bytes.find( "row0" ) == std::string::npos );
}

TEST_CASE( "a committed answer is a block of formatted rows", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "**bold** answer\nsecond row\n";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	// two rendered rows, not one span carrying an embedded newline.
	REQUIRE( coordinator.state( ).pending_commit.size( ) == 2 );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "bold answer" ) != std::string::npos );
	CHECK( bytes.find( "second row" ) != std::string::npos );
	CHECK( bytes.find( "**" ) == std::string::npos );
}

TEST_CASE( "a committed row wraps at word boundaries within the column budget",
	"[tui][transcript]" ) {
	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < 40; ++index ) {
		text += "word ";
	}

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( row_width( source.front( ) ) > 40 );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	for ( const auto& row : wrapped ) {
		CHECK( row_width( row ) <= 40 );

		// every row holds whole words: the split never lands mid-word.
		auto rest = row_text( row );

		while ( !rest.empty( ) ) {
			REQUIRE( rest.starts_with( "word" ) );
			rest.erase( 0, 4 );

			if ( rest.starts_with( " " ) ) {
				rest.erase( 0, 1 );
			}
		}
	}
}

TEST_CASE( "a wrapped bullet hangs its continuation rows under the text",
	"[tui][transcript]" ) {
	const auto text = std::string{ "- " } +
		"the quick brown fox jumps over the lazy dog and keeps on running";

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( source.front( ).size( ) == 2 );
	REQUIRE( source.front( ).front( ).text == "• " );
	REQUIRE( row_width( source.front( ) ) > 40 );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	// the marker is two columns wide, and each continuation repeats them as
	// spaces, so the text hangs under the bullet's body and not at column 0.
	for ( auto index = std::size_t{ 1 }; index < wrapped.size( ); ++index ) {
		REQUIRE_FALSE( wrapped[ index ].empty( ) );
		CHECK( wrapped[ index ].front( ).text == "  " );
		CHECK( row_width( wrapped[ index ] ) <= 40 );
	}
}

TEST_CASE( "a wrapped blockquote repeats its gutter on continuation rows",
	"[tui][transcript]" ) {
	const auto text = std::string{ "> " } +
		"the quick brown fox jumps over the lazy dog and keeps on running";

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( source.front( ).front( ).text == "│ " );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	for ( auto index = std::size_t{ 1 }; index < wrapped.size( ); ++index ) {
		REQUIRE_FALSE( wrapped[ index ].empty( ) );
		CHECK( wrapped[ index ].front( ).text == "│ " );
	}
}

TEST_CASE( "a bold span straddling the wrap point stays bold on both rows",
	"[tui][transcript]" ) {
	const auto row = styled_line{
		{ "start ", token::text },
		{ "the bold run crosses the wrap column here", token::text, token::none, true },
	};

	REQUIRE( row_width( row ) > 24 );

	const auto wrapped = transcript::wrap_row( row, 24, 1 );

	REQUIRE( wrapped.size( ) >= 2 );
	CHECK_FALSE( wrapped.front( ).front( ).bold );

	for ( const auto& line : wrapped ) {
		CHECK( row_bold( line ) );
		CHECK( row_width( line ) <= 24 );
	}
}

TEST_CASE( "a single word longer than the row splits on cluster boundaries",
	"[tui][transcript]" ) {
	const auto word = std::string( 100, 'w' );
	const auto row = styled_line{ { word, token::text } };

	const auto wrapped = transcript::wrap_row( row, 30, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	auto joined = std::string{ };

	for ( const auto& line : wrapped ) {
		CHECK( row_width( line ) <= 30 );
		joined += row_text( line );
	}

	// nothing is dropped and nothing overflows.
	CHECK( joined == word );
}

TEST_CASE( "a fenced code row is passed through unmodified", "[tui][transcript]" ) {
	const auto row = styled_line{ { std::string( 80, 'x' ), token::text, token::code_bg } };

	const auto wrapped = transcript::wrap_row( row, 20, 1 );

	REQUIRE( wrapped.size( ) == 1 );
	REQUIRE( wrapped.front( ).size( ) == 1 );
	CHECK( wrapped.front( ).front( ).text == row.front( ).text );
	CHECK( wrapped.front( ).front( ).background == token::code_bg );
	CHECK( row_width( wrapped.front( ) ) == 80 );
}

TEST_CASE( "the commit path wraps rows to the coordinator's own width",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	// one column stays reserved, so the commit wraps to 39 columns.
	coordinator.resize( 24, 40 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < 30; ++index ) {
		text += "token ";
	}

	coordinator.queue_text( text );

	const auto bytes = coordinator.flush( );

	auto start = bytes.find( "\x1b[0J" );

	REQUIRE( start != std::string::npos );
	start += 4;

	// at depth none the committed rows carry no escape, so the rows are the
	// text between the erase and the region's scroll.
	auto rows = std::vector< std::string >{ };

	while ( start < bytes.size( ) ) {
		const auto stop = bytes.find( "\r\n", start );

		if ( stop == std::string::npos ) {
			break;
		}

		rows.push_back( bytes.substr( start, stop - start ) );
		start = stop + 2;
	}

	REQUIRE( rows.size( ) > 1 );

	for ( const auto& row : rows ) {
		CHECK( string_width( row, 1 ) <= 39 );
	}
}

TEST_CASE( "a burst of deltas is rendered once per paint, not once per delta",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;

	// A line at a time, the way the provider streams. Re-rendering the whole
	// buffer per delta is quadratic in the answer's length, and that cost is
	// what made a long answer feel slow.
	const auto lines = std::size_t{ 500 };

	for ( auto index = std::size_t{ 0 }; index < lines; ++index ) {
		delta.text = "line " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	// the deltas only appended: nothing has re-rendered the buffer.
	CHECK( coordinator.stream_render_count( ) == 0 );

	const auto bytes = coordinator.flush( );

	// one paint, one whole-buffer render, whatever the delta count.
	CHECK( coordinator.stream_render_count( ) == 1 );

	// and the rows that paint built are the whole buffer, not the last delta.
	REQUIRE( coordinator.state( ).streaming_rows.size( ) == lines );
	CHECK( bytes.find( "line 499" ) != std::string::npos );

	// a paint with nothing new renders nothing at all: the caret is the only
	// thing it writes.
	const auto idle = coordinator.flush( );

	CHECK( idle == std::string{ "\x1b[3G" } );
	CHECK( coordinator.stream_render_count( ) == 1 );

	// the reasoning block is bounded the same way.
	delta.type = event_queue::kind::thinking_delta;

	const auto thoughts = std::size_t{ 100 };

	for ( auto index = std::size_t{ 0 }; index < thoughts; ++index ) {
		delta.text = "thought " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	CHECK( coordinator.stream_render_count( ) == 1 );

	const auto painted = coordinator.flush( );

	CHECK( coordinator.stream_render_count( ) == 2 );
	CHECK( coordinator.state( ).thinking_rows.size( ) == thoughts );

	// the region keeps the newest rows, so the newest thought is on screen.
	CHECK( painted.find( "thought 99" ) != std::string::npos );
}

TEST_CASE( "sizing a state materialises the block it holds", "[tui][render]" ) {
	// the region's height is read from the rows, so the sizing path is one of
	// the readers that has to bring them up to date: a height taken from
	// stale rows lags the text by a frame.
	auto state = render_state{ };
	state.streaming_text = "one\ntwo";
	state.streaming_stale = true;

	REQUIRE( state.streaming_rows.empty( ) );

	// prompt + status + two rows.
	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.streaming_rows.size( ) == 2 );
	CHECK( state.stream_render_count == 1 );

	// a second sizing is free: the rows already match the buffer.
	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.stream_render_count == 1 );

	// the frame builder is the same reader, so a stale state still paints its
	// buffer: it is the second caller of the one render path, not a second
	// render path.
	auto painted = render_state{ };
	painted.streaming_text = "stale but current";
	painted.streaming_stale = true;

	const auto frame = build_frame( painted, ROWS, COLUMNS, 1 );

	CHECK( painted.streaming_rows.size( ) == 1 );
	CHECK( painted.stream_render_count == 1 );
	CHECK( frame.rows( ) == ROWS );
}

TEST_CASE( "a tool call closes the block with the text streamed so far",
	"[tui][render]" ) {
	// no paint happened between the deltas and the call, so the commit has to
	// render the buffer it holds: committing a row cache the deltas left
	// behind would drop the prose.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "prose before the call\nsecond row of it";
	coordinator.apply( delta );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "bash";
	coordinator.apply( start );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "prose before the call" ) != std::string::npos );
	CHECK( bytes.find( "second row of it" ) != std::string::npos );

	// and the closed block is gone from the live region.
	CHECK( coordinator.state( ).streaming_text.empty( ) );
	CHECK( coordinator.state( ).streaming_rows.empty( ) );
}

TEST_CASE( "a turn end commits the buffer, not the row cache", "[tui][render]" ) {
	// the deltas left the rows stale and no paint ran between them and the
	// end of the turn, so the commit is the only thing that materialises the
	// block. Committing the stale cache would print nothing at all.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "**bold** answer\nsecond row\n";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	// two rows, formatted, and no markdown markers in the committed block.
	REQUIRE( coordinator.state( ).pending_commit.size( ) == 2 );
	CHECK( row_text( coordinator.state( ).pending_commit[ 0 ] ) == "bold answer" );
	CHECK( row_bold( coordinator.state( ).pending_commit[ 0 ] ) );
	CHECK( row_text( coordinator.state( ).pending_commit[ 1 ] ) == "second row" );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "**" ) == std::string::npos );
	CHECK( bytes.find( "bold answer" ) != std::string::npos );
}

TEST_CASE( "a thought closes with the reasoning streamed so far",
	"[tui][render]" ) {
	// the thought block commits from its rendered rows, and the deltas left
	// them stale: the commit path is what materialises them.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = "weighed the first option\nweighed the second";
	coordinator.apply( delta );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "read";
	coordinator.apply( start );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "weighed the first option" ) != std::string::npos );
	CHECK( bytes.find( "weighed the second" ) != std::string::npos );
}

TEST_CASE( "a render happens once per paint, whatever the delta rate",
	"[tui][render]" ) {
	// The pump's real shape: drain a tick's deltas, then paint once. The
	// bound is renders == paints, and every paint has to show the whole
	// buffer streamed so far -- not the tick's slice of it.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;

	const auto ticks = std::size_t{ 25 };
	const auto per_tick = std::size_t{ 20 };
	auto text = std::string{ };

	for ( auto tick = std::size_t{ 0 }; tick < ticks; ++tick ) {
		for ( auto index = std::size_t{ 0 }; index < per_tick; ++index ) {
			const auto piece = "word" + std::to_string( tick ) + " ";
			delta.text = piece;
			coordinator.apply( delta );
			text += piece;
		}

		// one render per paint, and the rows are the whole buffer.
		const auto painted = coordinator.flush( );

		REQUIRE( coordinator.stream_render_count( ) == tick + 1 );
		REQUIRE_FALSE( painted.empty( ) );
		REQUIRE( same_rows( coordinator.state( ).streaming_rows,
			transcript::render_block( text, token::text ) ) );
	}

	CHECK( coordinator.state( ).streaming_text == text );
}
