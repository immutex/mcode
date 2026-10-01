// The input editor against synthetic key events: multi-line, history, paste,
// autosuggest.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/editor.hxx"

using namespace mcode::tui;

namespace {

	auto character( const std::string_view text ) -> input_editor::key_event {
		auto event = input_editor::key_event{ };
		event.type = input_editor::key::character;
		event.text = std::string{ text };

		return event;
	}

	auto key( const input_editor::key type ) -> input_editor::key_event {
		auto event = input_editor::key_event{ };
		event.type = type;

		return event;
	}

}

TEST_CASE( "typing builds a line and enter submits it", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "fix " ) );
	std::ignore = editor.handle( character( "the test" ) );

	const auto submission = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( submission.has_value( ) );
	CHECK( *submission == "fix the test" );
	CHECK( editor.lines( ).size( ) == 1 );
	CHECK( editor.lines( ).front( ).text.empty( ) );
}

TEST_CASE( "shift-enter splits the line at the cursor", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::shift_enter ) );
	std::ignore = editor.handle( character( "cd" ) );

	REQUIRE( editor.lines( ).size( ) == 2 );
	CHECK( editor.lines( )[ 0 ].text == "ab" );
	CHECK( editor.lines( )[ 1 ].text == "cd" );

	const auto submission = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( submission.has_value( ) );
	CHECK( *submission == "ab\ncd" );
}

TEST_CASE( "backspace joins lines at the start of a row", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::shift_enter ) );
	std::ignore = editor.handle( character( "cd" ) );
	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::backspace ) );

	REQUIRE( editor.lines( ).size( ) == 1 );
	CHECK( editor.lines( ).front( ).text == "abcd" );
}

TEST_CASE( "history navigation walks up and returns to the draft", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "first" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );
	std::ignore = editor.handle( character( "second" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );

	std::ignore = editor.handle( character( "draft" ) );
	std::ignore = editor.handle( key( input_editor::key::up ) );

	CHECK( editor.lines( ).front( ).text == "second" );

	std::ignore = editor.handle( key( input_editor::key::up ) );

	CHECK( editor.lines( ).front( ).text == "first" );

	std::ignore = editor.handle( key( input_editor::key::down ) );

	CHECK( editor.lines( ).front( ).text == "second" );

	std::ignore = editor.handle( key( input_editor::key::down ) );

	CHECK( editor.lines( ).front( ).text == "draft" );
}

TEST_CASE( "a repeated history entry is not stored twice", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "same" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );
	std::ignore = editor.handle( character( "same" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );

	CHECK( editor.history( ).size( ) == 1 );
}

TEST_CASE( "the ghost suggestion completes from history", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "cmake --build build/Release" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );

	std::ignore = editor.handle( character( "cmake" ) );

	CHECK( editor.suggestion( ) == " --build build/Release" );
}

TEST_CASE( "no suggestion when nothing matches or the line is empty",
	"[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "zzz" ) );
	CHECK( editor.suggestion( ).empty( ) );

	std::ignore = editor.handle( key( input_editor::key::backspace ) );
	std::ignore = editor.handle( key( input_editor::key::backspace ) );
	std::ignore = editor.handle( key( input_editor::key::backspace ) );

	CHECK( editor.suggestion( ).empty( ) );
}

TEST_CASE( "interrupt clears the pending input and the session continues",
	"[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::shift_enter ) );
	std::ignore = editor.handle( character( "cd" ) );

	std::ignore = editor.handle( key( input_editor::key::interrupt ) );

	REQUIRE( editor.lines( ).size( ) == 1 );
	CHECK( editor.lines( ).front( ).text.empty( ) );
	CHECK_FALSE( editor.exit_requested( ) );
}

TEST_CASE( "cursor movement stays within the buffer", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "abc" ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.cursor_offset( ) == 1 );

	std::ignore = editor.handle( character( "X" ) );

	CHECK( editor.lines( ).front( ).text == "aXbc" );

	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.cursor_offset( ) == 0 );

	std::ignore = editor.handle( key( input_editor::key::end ) );

	CHECK( editor.cursor_offset( ) == 4 );
}

TEST_CASE( "delete removes forward and joins at the end of a row", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "abc" ) );
	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::delete_key ) );

	CHECK( editor.lines( ).front( ).text == "bc" );

	// Delete at the end of a row joins the next one into it.
	std::ignore = editor.handle( key( input_editor::key::end ) );
	std::ignore = editor.handle( key( input_editor::key::shift_enter ) );
	std::ignore = editor.handle( character( "def" ) );
	std::ignore = editor.handle( key( input_editor::key::up ) );
	std::ignore = editor.handle( key( input_editor::key::end ) );
	std::ignore = editor.handle( key( input_editor::key::delete_key ) );

	REQUIRE( editor.lines( ).size( ) == 1 );
	CHECK( editor.lines( ).front( ).text == "bcdef" );
}

TEST_CASE( "escape abandons the pending input without ending the session",
	"[tui][editor]" ) {
	// Escape used to map to the session-exit key, so one stray press lost the
	// whole conversation.
	auto editor = input_editor{ };

	auto typed = input_editor::key_event{ };
	typed.type = input_editor::key::character;
	typed.text = "half a thought";
	std::ignore = editor.handle( typed );

	CHECK( editor.text( ) == "half a thought" );

	auto escape = input_editor::key_event{ };
	escape.type = input_editor::key::escape;
	const auto submitted = editor.handle( escape );

	CHECK_FALSE( submitted.has_value( ) );
	CHECK( editor.text( ).empty( ) );
}

TEST_CASE( "the flattened caret counts across rows", "[tui][editor]" ) {
	// The one-line prompt echo needs a single offset, and it must match the
	// space `text()` joins rows with, or the caret drifts by one per row.
	auto editor = input_editor{ };

	auto first = input_editor::key_event{ };
	first.type = input_editor::key::character;
	first.text = "abc";
	std::ignore = editor.handle( first );

	auto newline = input_editor::key_event{ };
	newline.type = input_editor::key::shift_enter;
	std::ignore = editor.handle( newline );

	auto second = input_editor::key_event{ };
	second.type = input_editor::key::character;
	second.text = "de";
	std::ignore = editor.handle( second );

	CHECK( editor.text( ) == "abc de" );
	CHECK( editor.flattened_cursor( ) == 6 );
}
