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

	CHECK( editor.cursor_column( ) == 1 );

	std::ignore = editor.handle( character( "X" ) );

	CHECK( editor.lines( ).front( ).text == "aXbc" );

	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.cursor_column( ) == 0 );

	std::ignore = editor.handle( key( input_editor::key::end ) );

	CHECK( editor.cursor_column( ) == 4 );
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
