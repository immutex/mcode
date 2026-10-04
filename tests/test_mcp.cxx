#include <catch2/catch_test_macros.hpp>

#include "mcode/agent/loop.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/mcp/client.hxx"
#include "mcode/mcp/connect.hxx"
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
	CHECK( ( *frame )->id.number == 3 );
	CHECK_FALSE( ( *frame )->id.is_string );
}

TEST_CASE( "a string id marks a request, not a notification", "[mcp]" ) {
	auto request = jsonrpc::parse_line( R"({"jsonrpc":"2.0","id":"abc","method":"ping"})" );
	REQUIRE( static_cast< bool >( request ) );
	REQUIRE( request->has_value( ) );
	CHECK( ( *request )->kind == jsonrpc::message_kind::request );
	CHECK( ( *request )->id.is_string );
	CHECK( ( *request )->id.text == "abc" );

	auto notification = jsonrpc::parse_line( R"({"jsonrpc":"2.0","method":"notifications/x"})" );
	REQUIRE( static_cast< bool >( notification ) );
	REQUIRE( notification->has_value( ) );
	CHECK( ( *notification )->kind == jsonrpc::message_kind::notification );

	// a numeric-looking string id must correlate as text, never as the number
	auto numeric_text = jsonrpc::parse_line( R"({"jsonrpc":"2.0","id":"7","result":{}})" );
	REQUIRE( static_cast< bool >( numeric_text ) );
	REQUIRE( numeric_text->has_value( ) );
	CHECK( ( *numeric_text )->id.is_string );
	CHECK( ( *numeric_text )->id.text == "7" );
}

TEST_CASE( "an error reply carries error and never result", "[mcp]" ) {
	auto rendered = jsonrpc::render_error_response( jsonrpc::request_id::string( "srv-1" ),
		jsonrpc::METHOD_NOT_FOUND, "method not found" );
	REQUIRE( static_cast< bool >( rendered ) );
	CHECK( rendered->find( "\"error\"" ) != std::string::npos );
	CHECK( rendered->find( "\"result\"" ) == std::string::npos );
	CHECK( rendered->find( "\"id\":\"srv-1\"" ) != std::string::npos );

	auto numeric = jsonrpc::render_error_response( jsonrpc::request_id::numeric( 3 ),
		jsonrpc::INVALID_PARAMS, "bad params" );
	REQUIRE( static_cast< bool >( numeric ) );
	CHECK( numeric->find( "\"id\":3" ) != std::string::npos );
	CHECK( numeric->find( "\"result\"" ) == std::string::npos );

	auto success = jsonrpc::render_response( jsonrpc::request_id::numeric( 3 ), R"({"ok":true})" );
	REQUIRE( static_cast< bool >( success ) );
	CHECK( success->find( "\"result\"" ) != std::string::npos );
	CHECK( success->find( "\"error\"" ) == std::string::npos );
}

TEST_CASE( "a session keeps stdin open and reports EOF", "[mcp][session]" ) {
	auto spawned = proc::session::spawn( proc::session_options{ .executable = ECHO, .args = { }, .working_directory = { } } );
	REQUIRE( spawned );

	CHECK( spawned->running( ) );
	CHECK( spawned->id( ) != 0 );

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

TEST_CASE( "a server request with a string id is answered with an error", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "server-request" );

	// tools/list makes the server send its own request, which the client must answer
	auto tools = session_client->list_tools( );
	REQUIRE( static_cast< bool >( tools ) );

	const auto answered = wait_until( [ & ]( ) {
		session_client->pump( std::chrono::milliseconds{ 20 } );

		return wire->stderr_text( ).find( "reply-" ) != std::string::npos;
	} );

	REQUIRE( answered );
	CHECK( wire->stderr_text( ).find( "reply-error" ) != std::string::npos );
}

TEST_CASE( "a final frame without a newline is delivered", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "unterminated" );

	auto tools = session_client->list_tools( );
	REQUIRE( static_cast< bool >( tools ) );
	REQUIRE( tools->size( ) == 2 );
}

TEST_CASE( "an oversized unterminated line fails the transport, not memory", "[mcp]" ) {
	auto [ wire, session_client ] = handshaked_client( "oversized" );

	auto tools = session_client->list_tools( );
	REQUIRE_FALSE( static_cast< bool >( tools ) );
	CHECK( tools.error( ).code == errc::io );

	// the server is still blocked writing; terminating it keeps the test teardown short
	wire->stop( );
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

	board.on_transport_eof( );
	board.pump( std::chrono::milliseconds{ 0 } );

	REQUIRE( board.state( ) == server_state::ready );
	REQUIRE( saw_ready.size( ) == 2 );

	CHECK( registry.find( "mcp__echo__upper" ) != nullptr );
	CHECK( registry.find( "mcp__echo__count" ) != nullptr );
}

TEST_CASE( "a pending call is failed, not replayed", "[mcp][supervisor]" ) {
	auto [ wire, session_client ] = handshaked_client( "exit-mid-request" );

	auto pending = std::async( std::launch::async, [ & ]( ) {
		return session_client->call_tool( "upper", R"({"text":"x"})",
			DEFAULT_CALL_TIMEOUT );
	} );

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

	auto fresh = board.client_ptr( )->list_tools( );
	REQUIRE( fresh );

	CHECK( board.detect_changed_tools( *fresh ) );

	auto again = board.client_ptr( )->list_tools( );
	REQUIRE( again );
	CHECK_FALSE( board.detect_changed_tools( *again ) );
}

TEST_CASE( "a hung server times out with cancellation sent and late responses ignored",
	"[mcp]" ) {
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

	CHECK( elapsed < std::chrono::seconds{ 5 } );

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
	// a response arriving after cancellation lands on a retired id and is counted, never delivered
	auto [ wire, session_client ] = handshaked_client( "late" );

	auto timed = session_client->call_tool( "upper", R"({"text":"x"})",
		std::chrono::milliseconds{ 300 } );
	REQUIRE_FALSE( timed );
	CHECK( timed.error( ).code == errc::cancelled );

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

	auto report_text = wire->stderr_text( );
	REQUIRE_FALSE( report_text.empty( ) );
	CHECK( report_text.find( "trouble before the list" ) != std::string::npos );

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

	CHECK( mcp_source.unregister_server( "echo" ) == 2 );
	CHECK( registry.find( "mcp__echo__upper" ) == nullptr );
	CHECK( registry.size( ) == 0 );
}

TEST_CASE( "a server cannot mark itself read-only", "[mcp][source]" ) {
	// an mcp tool always registers as tool_class::mcp regardless of what the server claims
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

TEST_CASE( "the wiring registers a configured server's tools on the loop",
	"[mcp][connect]" ) {
	auto registry = tool_registry{ };
	auto servers = server_set{ };
	auto loop = mcode::agent_loop{ mcode::agent_loop::dependencies{ } };

	auto input = connect_input{ };
	input.registry = &registry;
	input.loop = &loop;
	input.owned = &servers;

	auto report = connect_list( { config_with( "normal" ) }, input );
	REQUIRE( report );

	CHECK( report->started.size( ) == 1 );
	CHECK( report->started.front( ) == "echo" );
	CHECK( report->failed.empty( ) );

	CHECK( registry.find( qualified_tool_name( "echo", "upper" ) ) != nullptr );
	CHECK( registry.find( qualified_tool_name( "echo", "count" ) ) != nullptr );

	const auto* handler = loop.handler_for( qualified_tool_name( "echo", "upper" ) );
	REQUIRE( handler != nullptr );

	auto outcome = ( *handler )( R"({"text":"hello"})" );
	REQUIRE( outcome );
	CHECK( outcome->find( "HELLO" ) != std::string::npos );
	CHECK( outcome->find( UNTRUSTED_BEGIN ) != std::string::npos );
	CHECK( outcome->find( UNTRUSTED_END ) != std::string::npos );

	servers.shutdown_all( );
}

TEST_CASE( "a disabled server is not started and a bad one does not fail the session",
	"[mcp][connect]" ) {
	auto registry = tool_registry{ };
	auto servers = server_set{ };
	auto loop = mcode::agent_loop{ mcode::agent_loop::dependencies{ } };

	auto disabled = config_with( "normal" );
	disabled.enabled = false;

	auto unstartable = config_with( "normal" );
	unstartable.name = "missing";
	unstartable.command = { "mcode-mcp-echo-not-on-path-xyz" };

	auto input = connect_input{ };
	input.registry = &registry;
	input.loop = &loop;
	input.owned = &servers;

	auto report = connect_list( { disabled, unstartable }, input );
	REQUIRE( report );

	CHECK( report->started.empty( ) );
	CHECK( registry.size( ) == 0 );
	CHECK( servers.all( ).empty( ) );

	CHECK( report->failed.size( ) == 1 );
	CHECK( report->failed.front( ) == "missing" );

	servers.shutdown_all( );
}

TEST_CASE( "a tool result comes back wrapped as untrusted data", "[mcp][connect]" ) {
	auto registry = tool_registry{ };
	auto servers = server_set{ };
	auto loop = mcode::agent_loop{ mcode::agent_loop::dependencies{ } };

	auto input = connect_input{ };
	input.registry = &registry;
	input.loop = &loop;
	input.owned = &servers;

	auto report = connect_list( { config_with( "normal" ) }, input );
	REQUIRE( report );

	const auto* handler = loop.handler_for( qualified_tool_name( "echo", "count" ) );
	REQUIRE( handler != nullptr );

	auto outcome = ( *handler )( R"({"text":"abcd"})" );
	REQUIRE( outcome );

	const auto expected = std::string{ UNTRUSTED_BEGIN } + "4" + std::string{ UNTRUSTED_END };
	CHECK( *outcome == expected );

	servers.shutdown_all( );
}

TEST_CASE( "no request can exceed the absolute maximum timeout", "[mcp][client]" ) {
	// `client::call` clamps a year-long request to ABSOLUTE_MAX_TIMEOUT, a shorter deadline wins
	auto [ wire, session_client ] = handshaked_client( "hang" );

	const auto started = std::chrono::steady_clock::now( );
	auto outcome = session_client->call_tool( "upper", R"({"text":"x"})",
		std::chrono::milliseconds{ ABSOLUTE_MAX_TIMEOUT } + std::chrono::hours{ 1 } );
	const auto elapsed = std::chrono::steady_clock::now( ) - started;

	REQUIRE_FALSE( outcome );
	CHECK( outcome.error( ).code == errc::cancelled );
	CHECK( elapsed < ABSOLUTE_MAX_TIMEOUT + std::chrono::seconds{ 10 } );

	auto short_call_started = std::chrono::steady_clock::now( );
	auto short_outcome = session_client->call_tool( "upper", R"({"text":"x"})",
		std::chrono::milliseconds{ 400 } );
	auto short_elapsed = std::chrono::steady_clock::now( ) - short_call_started;

	REQUIRE_FALSE( short_outcome );
	CHECK( short_elapsed < std::chrono::seconds{ 5 } );
}
