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
	CHECK( editor.text( ).empty( ) );
}

TEST_CASE( "a newline splits the line at the cursor", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::newline ) );
	std::ignore = editor.handle( character( "cd" ) );

	CHECK( editor.text( ) == "ab cd" );

	const auto submission = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( submission.has_value( ) );
	CHECK( *submission == "ab\ncd" );
}

TEST_CASE( "backspace joins lines at the start of a row", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::newline ) );
	std::ignore = editor.handle( character( "cd" ) );
	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::backspace ) );

	CHECK( editor.text( ) == "abcd" );
}

TEST_CASE( "history navigation walks up and returns to the draft", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "first" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );
	std::ignore = editor.handle( character( "second" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );

	std::ignore = editor.handle( character( "draft" ) );
	std::ignore = editor.handle( key( input_editor::key::up ) );

	CHECK( editor.text( ) == "second" );

	std::ignore = editor.handle( key( input_editor::key::up ) );

	CHECK( editor.text( ) == "first" );

	std::ignore = editor.handle( key( input_editor::key::down ) );

	CHECK( editor.text( ) == "second" );

	std::ignore = editor.handle( key( input_editor::key::down ) );

	CHECK( editor.text( ) == "draft" );
}

TEST_CASE( "interrupt clears the pending input", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "ab" ) );
	std::ignore = editor.handle( key( input_editor::key::newline ) );
	std::ignore = editor.handle( character( "cd" ) );

	std::ignore = editor.handle( key( input_editor::key::interrupt ) );

	CHECK( editor.text( ).empty( ) );
	CHECK( editor.flattened_cursor( ) == 0 );
}

TEST_CASE( "cursor movement stays within the buffer", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "abc" ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.flattened_cursor( ) == 1 );

	std::ignore = editor.handle( character( "X" ) );

	CHECK( editor.text( ) == "aXbc" );

	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.flattened_cursor( ) == 0 );

	std::ignore = editor.handle( key( input_editor::key::end ) );

	CHECK( editor.flattened_cursor( ) == 4 );
}

TEST_CASE( "delete removes forward and joins at the end of a row", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "abc" ) );
	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::delete_key ) );

	CHECK( editor.text( ) == "bc" );

	std::ignore = editor.handle( key( input_editor::key::end ) );
	std::ignore = editor.handle( key( input_editor::key::newline ) );
	std::ignore = editor.handle( character( "def" ) );
	std::ignore = editor.handle( key( input_editor::key::up ) );
	std::ignore = editor.handle( key( input_editor::key::end ) );
	std::ignore = editor.handle( key( input_editor::key::delete_key ) );

	CHECK( editor.text( ) == "bcdef" );
}

TEST_CASE( "escape abandons the pending input", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "half a thought" ) );

	CHECK( editor.text( ) == "half a thought" );

	const auto submitted = editor.handle( key( input_editor::key::escape ) );

	CHECK_FALSE( submitted.has_value( ) );
	CHECK( editor.text( ).empty( ) );
}

TEST_CASE( "a trailing backslash continues the line instead of submitting",
	"[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "line one\\" ) );

	const auto submitted = editor.handle( key( input_editor::key::enter ) );

	CHECK_FALSE( submitted.has_value( ) );
	CHECK( editor.row_count( ) == 2 );
	CHECK( editor.text( ) == "line one " );

	std::ignore = editor.handle( character( "line two" ) );

	CHECK( editor.row_count( ) == 2 );

	const auto completed = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( completed.has_value( ) );
	CHECK( *completed == "line one\nline two" );
}

TEST_CASE( "an escaped backslash submits with one literal backslash",
	"[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "path\\\\" ) );

	const auto submitted = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( submitted.has_value( ) );
	CHECK( *submitted == "path\\" );
	CHECK( editor.row_count( ) == 1 );
}

TEST_CASE( "enter with no backslash still submits", "[tui][editor]" ) {
	auto editor = input_editor{ };

	std::ignore = editor.handle( character( "plain" ) );

	const auto submitted = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( submitted.has_value( ) );
	CHECK( *submitted == "plain" );
}

TEST_CASE( "the history accessor exposes what was submitted, oldest first",
	"[tui][editor]" ) {
	auto editor = input_editor{ };

	CHECK( editor.history( ).empty( ) );

	std::ignore = editor.handle( character( "first" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );
	std::ignore = editor.handle( character( "second" ) );
	std::ignore = editor.handle( key( input_editor::key::enter ) );

	REQUIRE( editor.history( ).size( ) == 2 );
	CHECK( editor.history( )[ 0 ] == "first" );
	CHECK( editor.history( )[ 1 ] == "second" );
}

TEST_CASE( "the backslash gesture follows the caret, not the line end", "[tui][editor]" ) {
	auto editor = input_editor{ };

	// the caret sits between the two backslashes of `a\\b`, so the run before it
	// is one long: enter continues the line there rather than submitting it
	std::ignore = editor.handle( character( "a\\\\b" ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	const auto submitted = editor.handle( key( input_editor::key::enter ) );

	CHECK_FALSE( submitted.has_value( ) );
	CHECK( editor.row_count( ) == 2 );
	CHECK( editor.text( ) == "a \\b" );
}

TEST_CASE( "caret movement and erasing follow whole glyphs", "[tui][editor]" ) {
	auto editor = input_editor{ };

	// U+00E9 is two bytes in UTF-8, so a byte-wise caret splits it
	std::ignore = editor.handle( character( "\u00e9" ) );
	std::ignore = editor.handle( character( "x" ) );

	REQUIRE( editor.text( ) == "\u00e9x" );

	std::ignore = editor.handle( key( input_editor::key::backspace ) );

	CHECK( editor.text( ) == "\u00e9" );

	// the second backspace removes the whole glyph, not one of its bytes.
	std::ignore = editor.handle( key( input_editor::key::backspace ) );

	CHECK( editor.text( ).empty( ) );
	CHECK( editor.flattened_cursor( ) == 0 );

	std::ignore = editor.handle( character( "\u00e9" ) );
	std::ignore = editor.handle( key( input_editor::key::left ) );

	CHECK( editor.flattened_cursor( ) == 0 );

	std::ignore = editor.handle( key( input_editor::key::right ) );

	CHECK( editor.flattened_cursor( ) == 2 );

	// forward delete removes the whole glyph too.
	std::ignore = editor.handle( key( input_editor::key::home ) );
	std::ignore = editor.handle( key( input_editor::key::delete_key ) );

	CHECK( editor.text( ).empty( ) );
}


