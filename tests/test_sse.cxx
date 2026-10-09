#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mcode/net/sse.hxx"

using mcode::net::extract_delta_text;
using mcode::net::sse_event;
using mcode::net::sse_parser;

namespace {

	struct harness {
		std::vector< sse_event > events;
		sse_parser parser;

		harness( ) : parser{ [this]( sse_event&& event ) { events.push_back( std::move( event ) ); } } { }

		// `[[nodiscard]]`, and the tests that care about a refusal call the
		// parser directly; this helper is for the ones that do not.
		auto feed( const std::string_view chunk ) -> void { std::ignore = parser.feed( chunk ); }
	};

}

TEST_CASE( "a single complete event parses", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: hello\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "hello" );
	CHECK( captured.events[ 0 ].event == "message" );
}

TEST_CASE( "an event split across arbitrary chunk boundaries still parses", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: he" );
	captured.feed( "llo wor" );
	captured.feed( "ld\n" );
	captured.feed( "\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "hello world" );
}

TEST_CASE( "feeding one byte at a time yields identical events", "[sse]" ) {
	const auto stream = std::string{ "event: delta\ndata: abc\n\ndata: def\n\n" };
	auto captured = harness{ };

	for ( const auto character : stream ) {
		captured.feed( std::string_view{ &character, 1 } );
	}

	REQUIRE( captured.events.size( ) == 2 );
	CHECK( captured.events[ 0 ].event == "delta" );
	CHECK( captured.events[ 0 ].data == "abc" );
	CHECK( captured.events[ 1 ].data == "def" );
	CHECK( captured.events[ 1 ].event == "message" );
}

TEST_CASE( "multiple events in one chunk are all dispatched", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: one\n\ndata: two\n\ndata: three\n\n" );

	REQUIRE( captured.events.size( ) == 3 );
	CHECK( captured.events[ 0 ].data == "one" );
	CHECK( captured.events[ 1 ].data == "two" );
	CHECK( captured.events[ 2 ].data == "three" );
}

TEST_CASE( "multi-line data joins with newlines", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: line one\ndata: line two\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "line one\nline two" );
}

TEST_CASE( "comments and keep-alives produce no events", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( ": this is a comment\n\n" );
	captured.feed( ": keep-alive\n\n" );
	captured.feed( "data: real\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "real" );
}

TEST_CASE( "id and retry fields are captured", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "id: 42\nretry: 3000\ndata: x\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].id == "42" );
	CHECK( captured.events[ 0 ].retry == "3000" );
}

TEST_CASE( "CRLF line endings are handled", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: windows\r\n\r\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "windows" );
}

TEST_CASE( "a bare field with no colon is tolerated", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data\ndata: value\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "\nvalue" );
}

TEST_CASE( "a lone CR terminates a line", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: one\r\rdata: two\r\r" );

	REQUIRE( captured.events.size( ) == 2 );
	CHECK( captured.events[ 0 ].data == "one" );
	CHECK( captured.events[ 1 ].data == "two" );
}

TEST_CASE( "a CRLF split across chunks is one terminator", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: split\r" );
	captured.feed( "\n\r\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == "split" );
}

TEST_CASE( "an event still open at EOF is discarded", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data: last one" );
	CHECK( captured.events.empty( ) );

	captured.parser.finish( );

	CHECK( captured.events.empty( ) );
}

TEST_CASE( "only one leading space after the colon is stripped", "[sse]" ) {
	auto captured = harness{ };
	captured.feed( "data:  two spaces\n\n" );

	REQUIRE( captured.events.size( ) == 1 );
	CHECK( captured.events[ 0 ].data == " two spaces" );
}

TEST_CASE( "the event counter tracks dispatches", "[sse]" ) {
	auto captured = harness{ };
	CHECK( captured.parser.events_parsed( ) == 0 );

	captured.feed( "data: a\n\ndata: b\n\n" );

	CHECK( captured.parser.events_parsed( ) == 2 );
}

TEST_CASE( "the nested choices delta shape is extracted", "[sse]" ) {
	auto text = extract_delta_text( R"({"choices":[{"delta":{"content":"hi"}}]})" );
	REQUIRE( text );
	CHECK( *text == "hi" );
}

TEST_CASE( "the flat delta shape is extracted", "[sse]" ) {
	auto text = extract_delta_text( R"({"delta":"plain"})" );
	REQUIRE( text );
	CHECK( *text == "plain" );
}

TEST_CASE( "[DONE] is a terminator, not an error", "[sse]" ) {
	auto text = extract_delta_text( "[DONE]" );
	REQUIRE( text );
	CHECK( text->empty( ) );
}

TEST_CASE( "a payload with no text yields empty, not an error", "[sse]" ) {
	auto role_only = extract_delta_text( R"({"choices":[{"delta":{"role":"assistant"}}]})" );
	REQUIRE( role_only );
	CHECK( role_only->empty( ) );

	auto finish = extract_delta_text( R"({"choices":[{"finish_reason":"stop"}]})" );
	REQUIRE( finish );
	CHECK( finish->empty( ) );
}

TEST_CASE( "malformed JSON in a data payload is an error", "[sse]" ) {
	auto text = extract_delta_text( "{not json" );
	REQUIRE_FALSE( text );
	CHECK( text.error( ).code == mcode::errc::json );
}

TEST_CASE( "an over-long event is refused, not grown without bound", "[sse]" ) {
	// An event is only dispatched on a blank line, so a stream that never sends
	// one grew `current_.data` for as long as the server kept writing. The class
	// declared MAX_EVENT_BYTES for exactly this and never enforced it.
	auto events = std::vector< sse_event >{ };
	auto parser = sse_parser{ [ & ]( sse_event&& value ) { events.push_back( std::move( value ) ); } };

	// One `data:` line just over the cap, with no terminating blank line.
	auto payload = std::string( sse_parser::MAX_EVENT_BYTES + 1, 'x' );

	const auto fed = parser.feed( "data: " + payload + "\n" );

	REQUIRE_FALSE( static_cast< bool >( fed ) );
	CHECK( fed.error( ).code == mcode::errc::protocol );
	CHECK( events.empty( ) );
}

TEST_CASE( "a line with no terminator is refused past the line cap", "[sse]" ) {
	// The old code cleared the buffer and carried on, which left the parser
	// mid-stream: the rest of that line was re-read as a fresh field and the event
	// was assembled from the wrong bytes.
	auto events = std::vector< sse_event >{ };
	auto parser = sse_parser{ [ & ]( sse_event&& value ) { events.push_back( std::move( value ) ); } };

	const auto fed = parser.feed( std::string( sse_parser::MAX_LINE_BYTES + 1, 'y' ) );

	REQUIRE_FALSE( static_cast< bool >( fed ) );
	CHECK( fed.error( ).code == mcode::errc::protocol );
}

TEST_CASE( "an ordinary event still parses", "[sse]" ) {
	// The bound must not have broken the normal path.
	auto events = std::vector< sse_event >{ };
	auto parser = sse_parser{ [ & ]( sse_event&& value ) { events.push_back( std::move( value ) ); } };

	REQUIRE( static_cast< bool >( parser.feed( "data: {\"a\":1}\n\n" ) ) );

	REQUIRE( events.size( ) == 1 );
	CHECK( events.front( ).data == "{\"a\":1}" );
}
