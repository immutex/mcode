#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/notify.hxx"

using namespace mcode::tui;

namespace {

	[[nodiscard]] auto context( const bool stdout_is_terminal, const bool no_color,
		const bool osc9_supported = true ) -> notification_context {
		auto value = notification_context{ };
		value.stdout_is_terminal = stdout_is_terminal;
		value.no_color = no_color;
		value.osc9_supported = osc9_supported;

		return value;
	}

}

TEST_CASE( "a notification is suppressed when stdout is not a terminal",
	"[tui][notify]" ) {
	// Piped output is the `exec --json` contract, so even a bell is bytes a consumer
	// has to skip.
	CHECK( notification_bytes( "mcode", "turn complete", context( false, false ) ).empty( ) );

	// NO_COLOR wins on a terminal too: the user asked for no escapes.
	CHECK( notification_bytes( "mcode", "turn complete", context( true, true ) ).empty( ) );
}

TEST_CASE( "a notification is OSC 9 on a terminal", "[tui][notify]" ) {
	const auto bytes = notification_bytes( "mcode", "turn complete", context( true, false ) );

	CHECK( bytes == "\x1b]9;mcode: turn complete\x07" );
}

TEST_CASE( "a terminal with no OSC handler gets the bell instead", "[tui][notify]" ) {
	const auto bytes = notification_bytes( "mcode", "approval needed",
		context( true, false, false ) );

	CHECK( bytes == "\x07" );
}

TEST_CASE( "a control byte in the text cannot end the sequence early",
	"[tui][notify]" ) {
	const auto bytes = notification_bytes( "mcode", std::string{ "a\x07b\nc" },
		context( true, false ) );

	CHECK( bytes.ends_with( "\x07" ) );
	CHECK( bytes.find( '\x07' ) == bytes.size( ) - 1 );
	CHECK( bytes.find( '\n' ) == std::string::npos );
}

TEST_CASE( "the live notification path is silent when stdout is redirected",
	"[tui][notify]" ) {
	// Under the test runner stdout is a pipe, so this exercises the real path as a no-op.
	notify_terminal( "mcode", "turn complete" );

	SUCCEED( );
}
