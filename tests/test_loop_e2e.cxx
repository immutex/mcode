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
#include "mcode/perm/permission.hxx"
#include "mcode/perm/approval_headless.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"

#include "socket_test_helpers.hxx"
#include "test_scratch.hxx"


using namespace mcode;

namespace {


	using mcode::test::INVALID_SOCKET_HANDLE;
	using mcode::test::close_socket;
	using mcode::test::ensure_sockets;
	using mcode::test::host_to_network_long;
	using mcode::test::network_to_host_short;
	using mcode::test::receive_bytes;
	using mcode::test::send_bytes;
	using mcode::test::shutdown_socket;
	using mcode::test::socket_handle;

	// shutdown before joining so a test with fewer requests than scripted cannot block accept()
	class e2e_server {
	public:
		explicit e2e_server( std::vector< std::string > responses )
			: responses_( std::move( responses ) ) {
			ensure_sockets( );

			socket_ = ::socket( AF_INET, SOCK_STREAM, 0 );

			auto address = sockaddr_in{ };
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = host_to_network_long( INADDR_LOOPBACK );
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

			port_ = network_to_host_short( address.sin_port );
			worker_ = std::thread{ [this] { serve( ); } };
		}

		~e2e_server( ) {
			stopped_ = true;
			shutdown_socket( socket_ );
			close_socket( socket_ );

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

				if ( accepted == INVALID_SOCKET_HANDLE ) {
					return;
				}

				auto scratch = std::array< char, 8192 >{ };
				receive_bytes( accepted, scratch );

				const auto& body = responses_[ std::min( index, responses_.size( ) - 1 ) ];
				++index;

				auto head = std::string{ "HTTP/1.1 200 OK\r\n"
					"Content-Type: text/event-stream\r\nContent-Length: "
					+ std::to_string( body.size( ) ) + "\r\nConnection: close\r\n\r\n" };

				(void)send_bytes( accepted, head );
				(void)send_bytes( accepted, body );

				std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
				close_socket( accepted );
			}
		}

		std::vector< std::string > responses_;
		socket_handle socket_ = INVALID_SOCKET_HANDLE;
		std::uint16_t port_ = 0;
		std::thread worker_;
		std::atomic< bool > stopped_{ false };
	};

}

TEST_CASE( "exec drives a real read tool call over a loopback provider", "[loop][e2e]" ) {
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
	auto store = perm::remember_store{
		test::scratch_directory( "mcode-e2e-perm" ) / "permissions.json" };
	auto engine = perm::permission_engine{ *space, &store };
	auto headless = perm::headless_approval_source{ };

	{
		auto options = perm::permission_engine::options{ };
		options.headless = true;
		engine.set_options( options );
		engine.set_approval_source( &headless );
	}

	auto context = tools::tool_context{ };
	context.space = &*space;
	context.reads = &reads;
	context.permissions = &engine;
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

	auto found_content = false;
	auto found_call = false;
	auto result_is_correlated = false;

	for ( const auto& message : loop.history( ) ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_result
				&& block.result_json.find( marker ) != std::string::npos ) {
				found_content = true;

				// a tool result rendered with an empty id cannot be correlated by the model
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
