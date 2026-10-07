#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/tui/editor.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	// One read of the raw byte stream, through the same entry point the
	// consoles use.
	[[nodiscard]] auto feed( decode_state& state, const std::string_view bytes )
		-> std::vector< key_event > {
		auto out = std::vector< key_event >{ };

		decode_key_bytes( bytes, state, out, false );

		return out;
	}

	[[nodiscard]] auto count_of( const std::vector< key_event >& events,
		const key_event::kind kind ) -> std::size_t {
		auto total = std::size_t{ 0 };

		for ( const auto& event : events ) {
			if ( event.type == kind ) {
				++total;
			}
		}

		return total;
	}

	[[nodiscard]] auto first_paste( const std::vector< key_event >& events )
		-> std::string {
		for ( const auto& event : events ) {
			if ( event.type == key_event::kind::paste ) {
				return event.text;
			}
		}

		return { };
	}

}

TEST_CASE( "a multi-line paste arrives as one event, not as keystrokes",
	"[tui][paste]" ) {
	auto state = decode_state{ };

	const auto events = feed( state, "\x1b[200~first line\nsecond line\x1b[201~" );

	REQUIRE( events.size( ) == 1 );
	CHECK( events.front( ).type == key_event::kind::paste );
	CHECK( events.front( ).text == "first line\nsecond line" );

	// The bug this machine exists to prevent: the newline inside the paste
	// must not decode as a submission.
	CHECK( count_of( events, key_event::kind::enter ) == 0 );

	// Nor may a marker byte reach the input as a character.
	CHECK( count_of( events, key_event::kind::character ) == 0 );
}

TEST_CASE( "a paste split across reads still arrives as one event", "[tui][paste]" ) {
	auto state = decode_state{ };

	// The start marker in three reads, the content in two, the end marker in
	// two more.
	CHECK( feed( state, "\x1b[2" ).empty( ) );
	CHECK( feed( state, "0" ).empty( ) );
	CHECK( feed( state, "0~hello " ).empty( ) );
	CHECK( feed( state, "world\x1b[201" ).empty( ) );

	const auto events = feed( state, "~" );

	REQUIRE( events.size( ) == 1 );
	CHECK( events.front( ).type == key_event::kind::paste );
	CHECK( events.front( ).text == "hello world" );
}

TEST_CASE( "a terminal that sends no markers decodes exactly as before",
	"[tui][paste]" ) {
	auto state = decode_state{ };

	// An arrow key, typed characters and an Enter: nothing here is a marker,
	// and none of it is held back.
	const auto events = feed( state, "\x1b[Aok\r" );

	REQUIRE( events.size( ) == 4 );
	CHECK( events[ 0 ].type == key_event::kind::up );
	CHECK( events[ 1 ].text == "o" );
	CHECK( events[ 2 ].text == "k" );
	CHECK( events[ 3 ].type == key_event::kind::enter );
	CHECK( first_paste( events ).empty( ) );
}

TEST_CASE( "a wheel report is delivered only while mouse reporting is on",
	"[tui][paste]" ) {
	auto state = decode_state{ };
	auto out = std::vector< key_event >{ };

	// Mouse reporting is off outside a running turn, so the terminal is not
	// sending wheel reports at all; one that arrives anyway is swallowed
	// rather than typed into the input line.
	decode_key_bytes( "\x1b[<64;10;4M", state, out, false );

	CHECK( out.empty( ) );
	CHECK( first_paste( out ).empty( ) );

	auto reporting = std::vector< key_event >{ };

	decode_key_bytes( "\x1b[<64;10;4M", state, reporting, true );

	REQUIRE( reporting.size( ) == 1 );
	CHECK( reporting.front( ).type == key_event::kind::mouse_scroll_up );
}

TEST_CASE( "a lone escape is held and then delivered as the escape key",
	"[tui][paste]" ) {
	auto state = decode_state{ };

	// `ESC` opens both markers, so it waits for the next byte.
	CHECK( feed( state, "\x1b" ).empty( ) );
	CHECK( state.paste.holding( ) );

	// A byte that cannot continue a marker: both are delivered, in order.
	const auto events = feed( state, "q" );

	REQUIRE( events.size( ) == 2 );
	CHECK( events[ 0 ].type == key_event::kind::escape );
	CHECK( events[ 1 ].type == key_event::kind::character );
	CHECK( events[ 1 ].text == "q" );
}

TEST_CASE( "a wait that expires releases a held marker prefix", "[tui][paste]" ) {
	auto state = decode_state{ };
	auto out = std::vector< key_event >{ };

	// A lone Escape is a prefix of both markers, so it waits.
	CHECK( feed( state, "\x1b" ).empty( ) );
	CHECK( state.paste.holding( ) );

	// The wait expires: the held bytes go back through the keystroke decoder,
	// which holds the incomplete sequence in its own carry. The escape is not
	// lost, and the next call is what turns it into the escape key.
	decode_plain_key_bytes( state.paste.flush( ), state, out, false );

	CHECK_FALSE( state.paste.holding( ) );
	CHECK( out.empty( ) );
	CHECK( state.carry == "\x1b" );
}

TEST_CASE( "an open paste keeps its tail when a wait expires", "[tui][paste]" ) {
	auto state = decode_state{ };
	auto out = std::vector< key_event >{ };

	// An end marker that never arrives: the candidate is the front of one, so
	// it is kept rather than released. Releasing it would decode paste content
	// as keystrokes, which is the bug this machine exists to prevent.
	decode_key_bytes( "\x1b[200~kept\n\x1b[20", state, out, false );

	CHECK( out.empty( ) );
	CHECK( state.paste.active( ) );

	// What the idle path of `read_key` does: nothing comes out, and the
	// candidate is still there.
	CHECK( state.paste.flush( ).empty( ) );
	CHECK( state.paste.holding( ) );

	// The end marker finally completes, and the whole paste comes out.
	decode_key_bytes( "1~", state, out, false );

	REQUIRE( out.size( ) == 1 );
	CHECK( out.front( ).type == key_event::kind::paste );
	CHECK( out.front( ).text == "kept\n" );
}

TEST_CASE( "the first end marker closes the paste, whatever it contains",
	"[tui][paste]" ) {
	auto state = decode_state{ };

	const auto events = feed( state, "\x1b[200~a\x1b[201~b" );

	CHECK( first_paste( events ) == "a" );

	// What followed the paste is ordinary input: the stray marker is dropped
	// rather than typed, and the character after it is not.
	auto typed = std::string{ };

	for ( const auto& event : events ) {
		if ( event.type == key_event::kind::character ) {
			typed += event.text;
		}
	}

	CHECK( typed == "b" );
}

TEST_CASE( "a pasted carriage return becomes one newline", "[tui][paste]" ) {
	auto state = decode_state{ };

	// Windows pastes CRLF; the input buffer wants the line break, not the
	// carriage return.
	CHECK( first_paste( feed( state, "\x1b[200~one\r\ntwo\x1b[201~" ) ) == "one\ntwo" );
}

TEST_CASE( "a console reporting a paste one key at a time still gets one paste",
	"[tui][paste]" ) {
	// What the Windows console produces for a paste: the marker bytes as
	// individual key records, translated to bytes and fed one call each. This
	// is also a marker split into the worst possible reads.
	auto state = decode_state{ };
	auto out = std::vector< key_event >{ };

	for ( const auto byte : std::string{ "\x1b[200~line one\r\nline two\x1b[201~" } ) {
		decode_key_bytes( std::string_view{ &byte, 1 }, state, out, false );
	}

	REQUIRE( out.size( ) == 1 );
	CHECK( out.front( ).type == key_event::kind::paste );
	CHECK( out.front( ).text == "line one\nline two" );
}

TEST_CASE( "a byte stream with no markers keeps its escape and enter",
	"[tui][paste]" ) {
	auto state = decode_state{ };

	const auto events = feed( state, "\x1bz\r" );

	REQUIRE( events.size( ) == 3 );
	CHECK( events[ 0 ].type == key_event::kind::escape );
	CHECK( events[ 1 ].type == key_event::kind::character );
	CHECK( events[ 1 ].text == "z" );
	CHECK( events[ 2 ].type == key_event::kind::enter );
}

TEST_CASE( "a marker prefix that never completes goes back as ordinary input",
	"[tui][paste]" ) {
	auto state = decode_state{ };
	auto out = std::vector< key_event >{ };

	// A partial marker: the bytes are held, because the next read may finish
	// the marker.
	CHECK( feed( state, "\x1b[20" ).empty( ) );
	CHECK( state.paste.holding( ) );

	// The wait expires, so the held bytes are ordinary input after all. They
	// go back through the keystroke decoder, which holds the incomplete escape
	// sequence in its own carry and reports nothing yet: an unrecognised CSI
	// is dropped, which is what it did before the paste machine existed.
	decode_plain_key_bytes( state.paste.flush( ), state, out, false );

	CHECK_FALSE( state.paste.holding( ) );
	CHECK( out.empty( ) );
	CHECK( state.carry == "\x1b[20" );

	// The rest of a sequence arriving late is still a sequence, and still
	// decoded the same way: this one is not a key, so nothing is reported.
	decode_plain_key_bytes( "5~", state, out, false );

	CHECK( out.empty( ) );
	CHECK( state.carry.empty( ) );
}

TEST_CASE( "a pasted block is inserted into the input and not submitted",
	"[tui][paste]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( input_editor::key_event{
		input_editor::key::character, "ask: " } );

	// What the prompt loop does with a paste event.
	editor.insert_text( "one\ntwo\nthree" );

	// Three rows, and no submission: the block is content until Enter.
	CHECK( editor.row_count( ) == 3 );
	CHECK( editor.text( ) == "ask: one two three" );

	// The caret sits at the end of the pasted text, so typing continues there.
	std::ignore = editor.handle( input_editor::key_event{
		input_editor::key::character, "!" } );

	CHECK( editor.text( ) == "ask: one two three!" );

	const auto submission = editor.handle( input_editor::key_event{
		input_editor::key::enter, { } } );

	REQUIRE( submission.has_value( ) );
	CHECK( *submission == "ask: one\ntwo\nthree!" );
}

TEST_CASE( "a paste inserts at the caret, not at the end of the line",
	"[tui][paste]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( input_editor::key_event{
		input_editor::key::character, "ac" } );
	std::ignore = editor.handle( input_editor::key_event{
		input_editor::key::left, { } } );

	editor.insert_text( "b\nB" );

	CHECK( editor.row_count( ) == 2 );
	CHECK( editor.text( ) == "ab Bc" );
}

TEST_CASE( "a decoded paste feeds the editor without submitting",
	"[tui][paste]" ) {
	// The whole path in one test: bytes in, buffer out, nothing submitted.
	auto state = decode_state{ };
	auto editor = input_editor{ };

	const auto events = feed( state, "\x1b[200~line one\nline two\x1b[201~" );

	REQUIRE( events.size( ) == 1 );
	REQUIRE( events.front( ).type == key_event::kind::paste );

	editor.insert_text( events.front( ).text );

	// Two rows: the paste's newline became a row break, not a submission.
	CHECK( editor.row_count( ) == 2 );
	CHECK( editor.text( ) == "line one line two" );

	const auto submission = editor.handle( input_editor::key_event{
		input_editor::key::enter, { } } );

	REQUIRE( submission.has_value( ) );
	CHECK( *submission == "line one\nline two" );
}

TEST_CASE( "ctrl-c decodes to an interrupt, not a character", "[tui][keys]" ) {
	// The regression this guards: `read_line` had no interrupt case, so Ctrl-C
	// at a prompt that treats an empty answer as "skip" silently accepted the
	// empty string and carried on. Ctrl-C has to be distinguishable from an
	// empty line before any caller can act on it.
	auto state = decode_state{ };

	const auto events = feed( state, "\x03" );

	REQUIRE( events.size( ) == 1 );
	CHECK( events.front( ).type == key_event::kind::interrupt );
	CHECK( count_of( events, key_event::kind::character ) == 0 );

	// Ctrl-D is the exit key and is a different event.
	auto exit_state = decode_state{ };
	const auto exit_events = feed( exit_state, "\x04" );

	REQUIRE( exit_events.size( ) == 1 );
	CHECK( exit_events.front( ).type == key_event::kind::exit );
}
