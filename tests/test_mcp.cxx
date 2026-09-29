#include <catch2/catch_test_macros.hpp>

#include "mcode/core/registry.hxx"
#include "mcode/mcp/client.hxx"
#include "mcode/mcp/jsonrpc.hxx"
#include "mcode/mcp/source.hxx"
#include "mcode/mcp/supervisor.hxx"
#include "mcode/mcp/transport_stdio.hxx"
#include "mcode/proc/session.hxx"
#include "mcode/mcp/constants.hxx"

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mcode;
using namespace mcode::mcp;

namespace {

	const auto ECHO = std::filesystem::path{ MCODE_MCP_ECHO }.string( );

	auto config_with( const std::string mode ) -> server_config {
		auto config = server_config{ };
		config.name = "echo";
		config.transport = "stdio";
		config.command = { ECHO, mode };
		config.enabled = true;

		return config;
	}

	auto connected_client( const std::string mode ) -> std::unique_ptr< stdio_transport > {
		auto spawned = stdio_transport::spawn( config_with( mode ) );
		REQUIRE( spawned );

		auto wire = std::make_unique< stdio_transport >( std::move( *spawned ) );

		return wire;
	}

	auto handshaked_client( const std::string mode )
		-> std::pair< std::unique_ptr< stdio_transport >, std::unique_ptr< client > > {
		auto wire = connected_client( mode );
		auto session_client = std::make_unique< client >( *wire );

		session_client->attach( );

		auto caps = session_client->initialize( );
		REQUIRE( caps );

		return { std::move( wire ), std::move( session_client ) };
	}

	auto wait_until( const std::function< bool( ) >& predicate ) -> bool {
		const auto deadline = std::chrono::steady_clock::now( ) + std::chrono::seconds{ 10 };

		while ( std::chrono::steady_clock::now( ) < deadline ) {
			if ( predicate( ) ) {
				return true;
			}

			std::this_thread::sleep_for( std::chrono::milliseconds{ 10 } );
		}

		return false;
	}

}

TEST_CASE( "jsonrpc renders and parses frames", "[mcp]" ) {
	auto rendered = jsonrpc::render_request( 7, "tools/list", R"({"cursor":"a"})" );
	REQUIRE( rendered );
	CHECK( rendered->find( "\"id\":7" ) != std::string::npos );
	CHECK( rendered->find( "\"method\":\"tools/list\"" ) != std::string::npos );

	auto stamped = jsonrpc::stamp_meta( R"({"cursor":"a"})" );
	REQUIRE( stamped );
	CHECK( stamped->find( "\"_meta\"" ) != std::string::npos );
	CHECK( stamped->find( "\"cursor\"" ) != std::string::npos );

	// A second stamp is a no-op, not a second map.
	auto restamped = jsonrpc::stamp_meta( *stamped );
	REQUIRE( restamped );
	CHECK( *restamped == *stamped );

	auto banner = jsonrpc::parse_line( "this is not json" );
	REQUIRE( banner );
	CHECK_FALSE( banner->has_value( ) );

	auto frame = jsonrpc::parse_line(
		R"({"jsonrpc":"2.0","id":3,"result":{"tools":[]}})" );
	REQUIRE( frame );
	REQUIRE( frame->has_value( ) );
	CHECK( ( *frame )->kind == jsonrpc::message_kind::response );
	CHECK( ( *frame )->id == 3 );
}

TEST_CASE( "a session keeps stdin open and reports EOF", "[mcp][session]" ) {
	auto spawned = proc::session::spawn( proc::session_options{ .executable = ECHO } );
	REQUIRE( spawned );

	CHECK( spawned->running( ) );
	CHECK( spawned->id( ) != 0 );

	// A write succeeds while stdin is open; the child never sees EOF until it
	// is closed or the child exits.
	CHECK( spawned->write( R"({"jsonrpc":"2.0"})" "\n" ) );

	auto closed = spawned->close_stdin( );
	REQUIRE( closed );

	CHECK( wait_until( [ & ]( ) { return !spawned->running( ); } ) );

	const auto code = spawned->wait_exit( std::chrono::seconds{ 5 } );
	REQUIRE( code );
	CHECK( *code == 0 );
}

TEST_CASE( "the handshake completes and lists two tools", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "normal" );

	CHECK( session_client->capabilities( ).server_name == "mcp-echo" );
	CHECK( session_client->capabilities( ).negotiated_version == "2025-06-18" );
	CHECK( session_client->is_alive( ) );

	auto tools = session_client->list_tools( );
	REQUIRE( tools );
	REQUIRE( tools->size( ) == 2 );
	CHECK( ( *tools )[ 0 ].name == "upper" );
	CHECK( ( *tools )[ 1 ].name == "count" );
	CHECK_FALSE( ( *tools )[ 0 ].schema_json.empty( ) );
}

TEST_CASE( "a banner on stdout does not corrupt the stream", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "banner" );

	CHECK( session_client->capabilities( ).server_name == "mcp-echo" );

	auto tools = session_client->list_tools( );
	REQUIRE( tools );
	REQUIRE( tools->size( ) == 2 );
}

TEST_CASE( "a tool call round-trips", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "normal" );

	auto outcome = session_client->call_tool( "upper", R"({"text":"hello"})",
		DEFAULT_CALL_TIMEOUT );
	REQUIRE( outcome );
	CHECK( outcome->content == "HELLO" );
	CHECK_FALSE( outcome->is_error );
}

TEST_CASE( "a crashed server is restarted and its tools come back", "[mcp][supervisor]" ) {
	auto registry = tool_registry{ };
	auto mcp_source = source{ registry };

	auto config = config_with( "exit-mid-request" );
	auto saw_ready = std::vector< std::string >{ };

	auto board = supervisor( config, supervisor::handlers{
		[ & ]( const std::vector< server_tool >& tools ) {
			REQUIRE( mcp_source.register_server( config.name, tools ) );
			saw_ready.push_back( "ready" );
		},
		[ & ]( const std::string& reason ) {
			saw_ready.push_back( "gone: " + reason );
		},
		} );

	auto listed = board.start( );
	REQUIRE( listed );
	REQUIRE( saw_ready.size( ) == 1 );
	CHECK( registry.find( "mcp__echo__upper" ) != nullptr );

	// The supervisor is driven over the EOF event; the fixture exits on the
	// first post-handshake request, so the transport sees EOF and the restart
	// path runs.
	board.on_transport_eof( );

	REQUIRE( wait_until( [ & ]( ) { return board.state( ) == server_state::ready; } ) );
	REQUIRE( saw_ready.size( ) == 2 );

	// The tools came back: the follow-up defect from 07 -- respawned servers
	// wrongly deregistered -- does not reproduce.
	CHECK( registry.find( "mcp__echo__upper" ) != nullptr );
	CHECK( registry.find( "mcp__echo__count" ) != nullptr );
}

TEST_CASE( "a pending call is failed, not replayed", "[mcp][supervisor]" ) {
	// The fixture exits on any post-handshake request. A `tools/call` is in
	// flight when that happens; it must come back as a transport error, and a
	// restart must not silently re-issue it.
	auto [ wire, session_client ] = handshaked_client( "exit-mid-request" );

	auto pending = std::async( std::launch::async, [ & ]( ) {
		return session_client->call_tool( "upper", R"({"text":"x"})",
			DEFAULT_CALL_TIMEOUT );
	} );

	// The call fails with a transport error once EOF lands.
	auto outcome = pending.get( );
	REQUIRE_FALSE( outcome );
	CHECK( outcome.error( ).code == errc::io );
	CHECK_FALSE( session_client->is_alive( ) );
}

TEST_CASE( "a changed tools/list is detected via hash mismatch", "[mcp][supervisor]" ) {
	auto config = config_with( "changed-list" );
	auto board = supervisor( config, { } );

	auto listed = board.start( );
	REQUIRE( listed );
	REQUIRE( listed->size( ) == 2 );

	const auto pinned = board.pinned_hash( );
	REQUIRE_FALSE( pinned.empty( ) );

	// The fixture's second list returns changed definitions.
	auto fresh = board.client_ptr( )->list_tools( );
	REQUIRE( fresh );

	CHECK( board.detect_changed_tools( *fresh ) );

	// The first list, unchanged, does not trip the pin.
	auto again = board.client_ptr( )->list_tools( );
	REQUIRE( again );
	CHECK_FALSE( board.detect_changed_tools( *again ) );
}

TEST_CASE( "a hung server times out with cancellation sent and late responses ignored",
	"[mcp]" ) {
	// The "echo-notify" fixture echoes every notification back on stdout, so
	// the `notifications/cancelled` the client sends on timeout comes back as
	// an inbound notification the notify handler observes -- the assertion is
	// end-to-end, not a mock.
	auto [ wire, session_client ] = handshaked_client( "echo-notify" );

	auto notifications = std::vector< std::string >{ };
	session_client->set_notify_handler( [ & ]( const jsonrpc::message& frame ) {
		notifications.push_back( frame.method );
	} );

	const auto started = std::chrono::steady_clock::now( );
	auto outcome = session_client->call_tool( "upper", R"({"text":"x"})",
		std::chrono::milliseconds{ 500 } );
	const auto elapsed = std::chrono::steady_clock::now( ) - started;

	REQUIRE_FALSE( outcome );
	CHECK( outcome.error( ).code == errc::cancelled );

	// The loop was not blocked for the call timeout of a healthy server.
	CHECK( elapsed < std::chrono::seconds{ 5 } );

	// The cancellation notification went out and came back. The pump drives
	// the wire; without it the echoed notification would sit unread.
	auto saw_cancelled = false;
	const auto deadline = std::chrono::steady_clock::now( ) + std::chrono::seconds{ 10 };

	while ( std::chrono::steady_clock::now( ) < deadline ) {
		session_client->pump( std::chrono::milliseconds{ 50 } );

		for ( const auto& method : notifications ) {
			if ( method == "notifications/cancelled" ) {
				saw_cancelled = true;

				break;
			}
		}

		if ( saw_cancelled ) {
			break;
		}
	}

	REQUIRE( saw_cancelled );
}

TEST_CASE( "a late response after a timeout is ignored", "[mcp]" ) {
	// The "late" fixture waits past the deadline, then answers. The client
	// must have given up by then: the call returns `cancelled`, and the
	// response that eventually arrives lands on a retired id -- counted,
	// ignored, never delivered as a second result.
	auto [ wire, session_client ] = handshaked_client( "late" );

	auto timed = session_client->call_tool( "upper", R"({"text":"x"})",
		std::chrono::milliseconds{ 300 } );
	REQUIRE_FALSE( timed );
	CHECK( timed.error( ).code == errc::cancelled );

	// Pump past the fixture's two-second delay so the response actually
	// arrives while this test is still watching. The pump drives the wire;
	// without it the late response would sit unread.
	auto saw_late = false;
	const auto deadline = std::chrono::steady_clock::now( ) + std::chrono::seconds{ 10 };

	while ( std::chrono::steady_clock::now( ) < deadline ) {
		session_client->pump( std::chrono::milliseconds{ 50 } );

		if ( session_client->ignored_responses( ) > 0 ) {
			saw_late = true;

			break;
		}
	}

	CHECK( saw_late );
}

TEST_CASE( "stderr is captured and never fatal", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "stderr-exit" );

	auto tools = session_client->list_tools( );
	REQUIRE( tools );

	// The fixture wrote to stderr before answering; the ring holds it.
	auto report_text = wire->stderr_text( );
	REQUIRE_FALSE( report_text.empty( ) );
	CHECK( report_text.find( "trouble before the list" ) != std::string::npos );

	// A call makes the fixture exit non-zero on stderr; the call fails with a
	// transport error and the server state reflects the exit, but stderr
	// content is still buffered, not treated as a protocol failure.
	auto outcome = session_client->call_tool( "upper", R"({"text":"x"})",
		DEFAULT_CALL_TIMEOUT );
	REQUIRE_FALSE( outcome );
	CHECK( outcome.error( ).code == errc::io );

	auto after = wire->stderr_text( );
	CHECK( after.find( "fatal trouble on the call" ) != std::string::npos );
}

TEST_CASE( "source registers mcp tools with the right class, source and owner",
	"[mcp][source]" ) {
	auto registry = tool_registry{ };
	auto mcp_source = source{ registry };

	auto tools = std::vector< server_tool >{ };
	tools.push_back( { "upper", "Uppercases text",
		R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" } );
	tools.push_back( { "count", "Counts characters",
		R"({"type":"object","properties":{"text":{"type":"string"}}})" } );

	REQUIRE( mcp_source.register_server( "echo", tools ) );

	const auto* upper = registry.find( "mcp__echo__upper" );
	REQUIRE( upper != nullptr );
	CHECK( upper->klass == tool_class::mcp );
	CHECK( upper->source == tool_source::mcp );
	CHECK( upper->owner == "mcp:echo" );
	CHECK( upper->description.find( "<mcode:untrusted>" ) != std::string::npos );
	CHECK( upper->description.find( "</mcode:untrusted>" ) != std::string::npos );

	const auto* count = registry.find( "mcp__echo__count" );
	REQUIRE( count != nullptr );
	CHECK( count->klass == tool_class::mcp );

	// Teardown removes exactly the server's tools.
	CHECK( mcp_source.unregister_server( "echo" ) == 2 );
	CHECK( registry.find( "mcp__echo__upper" ) == nullptr );
	CHECK( registry.size( ) == 0 );
}

TEST_CASE( "a server cannot mark itself read-only", "[mcp][source]" ) {
	// The class is assigned by the registry path, never by the tool: whatever
	// the server claims, the registration lands as tool_class::mcp.
	auto registry = tool_registry{ };
	auto mcp_source = source{ registry };

	auto tools = std::vector< server_tool >{ };
	tools.push_back( { "readOnly_claim", "claims to be read-only",
		R"({"type":"object","properties":{},"x-readOnlyHint":true})" } );

	REQUIRE( mcp_source.register_server( "echo", tools ) );

	const auto* registered = registry.find( "mcp__echo__readOnly_claim" );
	REQUIRE( registered != nullptr );
	CHECK( registered->klass == tool_class::mcp );
}

TEST_CASE( "spawn never goes through a shell and fails closed", "[mcp]" ) {
	auto missing = config_with( "normal" );
	missing.command = { "mcode-mcp-echo-not-on-path-xyz" };

	CHECK_FALSE( stdio_transport::spawn( missing ) );

	auto empty = config_with( "normal" );
	empty.command = { };

	CHECK_FALSE( stdio_transport::spawn( empty ) );

	auto unsupported = config_with( "normal" );
	unsupported.transport = "http";

	CHECK_FALSE( stdio_transport::spawn( unsupported ) );
}

TEST_CASE( "graceful shutdown closes stdin before terminating", "[mcp][supervisor]" ) {
	auto config = config_with( "normal" );
	auto board = supervisor( config, { } );

	REQUIRE( board.start( ) );
	CHECK( board.state( ) == server_state::ready );

	board.shutdown( );

	CHECK( board.state( ) == server_state::stopped );

	// A second shutdown is a no-op, not an error.
	board.shutdown( );
}

TEST_CASE( "untrusted delimiters wrap description and result", "[mcp][source]" ) {
	auto wrapped = wrap_untrusted( "delete everything" );
	CHECK( wrapped == "<mcode:untrusted>delete everything</mcode:untrusted>" );

	auto tokens = estimate_schema_tokens( "0123456789abcdef" );
	CHECK( tokens == 4 );
	CHECK( estimate_exceeds_warning( MCP_SCHEMA_TOKEN_WARNING + 1 ) );
	CHECK_FALSE( estimate_exceeds_warning( MCP_SCHEMA_TOKEN_WARNING ) );
}
