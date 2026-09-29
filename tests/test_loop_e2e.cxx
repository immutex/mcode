// The merge acceptance test: a real turn in which the model calls the real
// `read` tool over a real HTTP round trip and receives real file content.

// The ReAct loop, driven by a scripted fake client.
//
// The loop's correctness is the state machine's, not the transport's, so every
// test here drives model_client with queued events and asserts on the visited
// state sequence -- never on a network.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"

#include "test_scratch.hxx"

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mcode;

namespace {

#if defined( _WIN32 )
	using e2e_socket = SOCKET;
	inline constexpr e2e_socket E2E_INVALID_SOCKET = INVALID_SOCKET;
#else
	using e2e_socket = int;
	inline constexpr e2e_socket E2E_INVALID_SOCKET = -1;
#endif

	auto e2e_close_socket( const e2e_socket handle ) -> void {
#if defined( _WIN32 )
		::closesocket( handle );
#else
		::close( handle );
#endif
	}

	auto e2e_shutdown_socket( const e2e_socket handle ) -> void {
#if defined( _WIN32 )
		::shutdown( handle, SD_BOTH );
#else
		::shutdown( handle, SHUT_RDWR );
#endif
	}

	// One SSE response per accepted connection, in order, on loopback. The
	// teardown shuts the listening socket down before joining so a test that made
	// fewer requests than scripted never blocks in accept().
	class e2e_server {
	public:
		explicit e2e_server( std::vector< std::string > responses )
			: responses_( std::move( responses ) ) {
		#if defined( _WIN32 )
			auto data = WSADATA{ };
			::WSAStartup( MAKEWORD( 2, 2 ), &data );
		#endif

			socket_ = ::socket( AF_INET, SOCK_STREAM, 0 );

			auto address = sockaddr_in{ };
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = ::htonl( INADDR_LOOPBACK );
			address.sin_port = 0;

			if ( ::bind( socket_, reinterpret_cast< sockaddr* >( &address ), sizeof( address ) ) != 0
				|| ::listen( socket_, 4 ) != 0 ) {
				return;
			}

			auto length = static_cast< socklen_t >( sizeof( address ) );

			if ( ::getsockname( socket_, reinterpret_cast< sockaddr* >( &address ), &length )
				!= 0 ) {
				return;
			}

			port_ = ::ntohs( address.sin_port );
			worker_ = std::thread{ [this] { serve( ); } };
		}

		~e2e_server( ) {
			stopped_ = true;
			e2e_shutdown_socket( socket_ );
			e2e_close_socket( socket_ );

			if ( worker_.joinable( ) ) {
				worker_.join( );
			}

		#if defined( _WIN32 )
			::WSACleanup( );
		#endif
		}

		e2e_server( const e2e_server& ) = delete;
		auto operator=( const e2e_server& ) -> e2e_server& = delete;

		[[nodiscard]] auto endpoint( ) const -> std::string {
			return "http://127.0.0.1:" + std::to_string( port_ ) + "/v1/chat/completions";
		}

	private:
		auto serve( ) -> void {
			auto index = std::size_t{ 0 };

			while ( !stopped_ ) {
				const auto accepted = ::accept( socket_, nullptr, nullptr );

				if ( accepted == E2E_INVALID_SOCKET ) {
					return;
				}

				auto scratch = std::array< char, 8192 >{ };
				::recv( accepted, scratch.data( ), static_cast< int >( scratch.size( ) ), 0 );

				const auto& body = responses_[ std::min( index, responses_.size( ) - 1 ) ];
				++index;

				auto head = std::string{ "HTTP/1.1 200 OK\r\n"
					"Content-Type: text/event-stream\r\nContent-Length: "
					+ std::to_string( body.size( ) ) + "\r\nConnection: close\r\n\r\n" };

				::send( accepted, head.data( ), static_cast< int >( head.size( ) ), 0 );
				::send( accepted, body.data( ), static_cast< int >( body.size( ) ), 0 );

				std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
				e2e_close_socket( accepted );
			}
		}

		std::vector< std::string > responses_;
		e2e_socket socket_ = E2E_INVALID_SOCKET;
		std::uint16_t port_ = 0;
		std::thread worker_;
		std::atomic< bool > stopped_{ false };
	};

}

// The merge acceptance test: `exec` drives a real turn in which the model calls
// the real `read` tool over a real HTTP round trip and receives real file
// content. Not a state sequence, not a schema -- the file's text comes back.
TEST_CASE( "exec drives a real read tool call over a loopback provider", "[loop][e2e]" ) {
	// The tests run from the build tree, so the workspace is a temp directory
	// with one real file the tool reads from disk.
	const auto fixture_root = test::scratch_directory( "mcode-loop-e2e" );

	const auto marker = std::string{ "marker-e2e-9137" };
	const auto fixture_name = std::string{ "notes.txt" };

	{
		auto file = std::ofstream{ fixture_root / fixture_name };
		file << "first line\n" << marker << "\nlast line\n";
	}

	auto space = workspace::open( fixture_root );
	REQUIRE( space.has_value( ) );

	const auto plan_turn = std::string{ "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\","
		"\"content\":\"Planning the read.\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":5}}\n\n"
		"data: [DONE]\n\n" };

	const auto turn_one = std::string{ "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\","
		"\"content\":\"\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
		"\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
		"{\"arguments\":\"{\\\"path\\\": \\\"notes.txt\\\"}\"}}]}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":10}}\n\n"
		"data: [DONE]\n\n" };

	const auto turn_two = std::string{ "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\","
		"\"content\":\"done\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200,\"completion_tokens\":5}}\n\n"
		"data: [DONE]\n\n" };

	auto server = e2e_server{ { plan_turn, turn_one, turn_two } };

	auto descriptor = model::provider_descriptor{ };
	descriptor.name = "openai-chat-completions";
	descriptor.endpoint = server.endpoint( );
	descriptor.auth.from = model::auth_spec::source::none;
	descriptor.stream.text_delta = "/choices/0/delta/content";
	descriptor.stream.tool_call_index = "/choices/0/delta/tool_calls/0/index";
	descriptor.stream.tool_call_id = "/choices/0/delta/tool_calls/0/id";
	descriptor.stream.tool_call_name = "/choices/0/delta/tool_calls/0/function/name";
	descriptor.stream.tool_call_args = "/choices/0/delta/tool_calls/0/function/arguments";
	descriptor.stream.finish_reason = "/choices/0/finish_reason";
	descriptor.stream.usage_input = "/usage/prompt_tokens";
	descriptor.stream.usage_output = "/usage/completion_tokens";

	const auto caps = model::lookup_capabilities( "gpt-5-mini" );
	REQUIRE( caps.has_value( ) );

	auto registry = tool_registry{ };
	auto log = event_log{ };
	auto bus = events::bus{ };
	auto transport = net::http_client{ };
	auto client = model::http_model_client{ transport };

	auto deps = agent_loop::dependencies{ };
	deps.registry = &registry;
	deps.client = &client;
	deps.log = &log;
	deps.bus = &bus;
	deps.model_name = "gpt-5-mini";
	deps.caps = *caps;
	deps.provider = descriptor;
	deps.workspace_root = space->root( ).string( );
	deps.platform_name = "test";

	auto loop = agent_loop{ deps };

	auto reads = tools::session_reads{ };
	auto policy = tools::exec_policy{ };
	auto context = tools::tool_context{ };
	context.space = &*space;
	context.reads = &reads;
	context.policy = &policy;
	context.run_id = "e2e";
	context.headless = true;

	auto sink = tools::vector_sink{ };
	const auto registered = tools::register_core_tools( registry, sink, context );
	REQUIRE( registered.has_value( ) );

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	const auto outcome = loop.run( "read notes.txt" );

	REQUIRE( outcome.has_value( ) );
	CHECK( outcome->final_state == loop_state::handoff );

	// The tool result in the history carries the file's real content, read from
	// disk by the real handler -- not a stub, not a schema, not a state list.
	auto found_content = false;
	auto found_call = false;
	auto result_is_correlated = false;

	for ( const auto& message : loop.history( ) ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_result
				&& block.result_json.find( marker ) != std::string::npos ) {
				found_content = true;

				// A tool result the model cannot correlate with the call it answers
				// is a result it cannot use. Rendered with an empty id the model
				// reports the result as missing and re-issues the call -- observed
				// live, and invisible to a test that only checks the content.
				result_is_correlated = block.tool_call_id == "call_1";
			}

			if ( block.kind == model::block_kind::tool_call
				&& block.tool_name == "read" && block.tool_call_id == "call_1" ) {
				found_call = true;
			}
		}
	}

	CHECK( found_call );

	CHECK( found_content );

	CHECK( result_is_correlated );

	auto error_code = std::error_code{ };
	std::filesystem::remove_all( fixture_root, error_code );
}
