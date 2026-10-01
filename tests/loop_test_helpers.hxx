#pragma once

// Shared by test_loop.cxx only. Extracted when that file passed the
// 600-line limit; the end-to-end test in test_loop_e2e.cxx drives the real
// tool registry and shares none of it.

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


namespace loop_test {

	using namespace mcode;


	inline constexpr std::int64_t TEST_CONTEXT_WINDOW = 200'000;

	// A scripted client: one queued response per stream() call. Records every
	// request it saw, so tests can assert on call counts and on request bytes.
	class scripted_client final : public model::model_client {
	public:
		// One scripted turn: the canonical events to emit, in order.
		struct response {
			std::string text;
			std::vector< mcode::tool_call > calls;
			std::int64_t input_tokens = 100;
			std::int64_t output_tokens = 50;
			bool fail = false;
		};

		auto queue( response value ) -> void {
			responses_.push_back( std::move( value ) );
		}

		auto stream( const model::stream_request& request, const model::event_sink& sink )
			-> status override {
			++calls_;
			requests_.push_back( request.request );

			if ( index_ >= responses_.size( ) ) {
				return std::unexpected( fail( errc::protocol, "script exhausted" ) );
			}

			const auto& scripted = responses_[ index_++ ];

			if ( scripted.fail ) {
				return std::unexpected( fail( errc::protocol, "scripted hard error" ) );
			}

			if ( scripted.input_tokens > 0 ) {
				auto usage = model::chat_event{ };
				usage.type = model::chat_event::kind::usage;
				usage.input_tokens = scripted.input_tokens;
				usage.output_tokens = scripted.output_tokens;
				sink( usage );
			}

			if ( !scripted.text.empty( ) ) {
				auto delta = model::chat_event{ };
				delta.type = model::chat_event::kind::text_delta;
				delta.text = scripted.text;
				sink( delta );
			}

			for ( const auto& call : scripted.calls ) {
				auto fragment = model::chat_event{ };
				fragment.type = model::chat_event::kind::tool_call_delta;
				fragment.index = static_cast< int >( emitted_calls_ );
				fragment.tool_call_id = "call-" + std::to_string( emitted_calls_ );
				fragment.tool_name = call.name;
				fragment.args_fragment = call.args_json;
				sink( fragment );
				++emitted_calls_;
			}

			auto done = model::chat_event{ };
			done.type = model::chat_event::kind::turn_done;
			sink( done );

			return status{ };
		}

		[[nodiscard]] auto call_count( ) const noexcept -> std::size_t { return calls_; }
		[[nodiscard]] auto request( const std::size_t index ) const -> const model::chat_request& {
			return requests_[ index ];
		}

	private:
		std::vector< response > responses_;
		std::vector< model::chat_request > requests_;
		std::size_t index_ = 0;
		std::size_t calls_ = 0;
		int emitted_calls_ = 0;
	};

	inline auto state_names( const std::vector< loop_state >& states ) -> std::string {
		auto out = std::string{ };

		for ( const auto value : states ) {
			if ( !out.empty( ) ) {
				out += ",";
			}

			out += to_string( value );
		}

		return out;
	}

	struct fixture {
		scripted_client client;
		tool_registry registry;
		event_log log;

		// The loop publishes here. A test that asserts on an event subscribes
		// to this; the loop owns nothing it is handed.
		events::bus bus;

		agent_loop::dependencies deps;
		std::unique_ptr< agent_loop > loop;

		fixture( ) {
			deps.client = &client;
			deps.registry = &registry;
			deps.log = &log;
			deps.bus = &bus;
			deps.model_name = "test-model";
			deps.caps.context_window = TEST_CONTEXT_WINDOW;
			deps.caps.price_input = 1.0;
			deps.caps.price_output = 2.0;
			deps.caps.caching = model::cache_mode::implicit;

			auto definition = tool_def{ };
			definition.name = "echo";
			definition.description = "echoes its argument";
			definition.schema_json = R"({"type":"object"})";

			std::ignore = registry.add( definition );

			auto shell = tool_def{ };
			shell.name = "bash";
			shell.description = "runs a command";
			shell.schema_json = R"({"type":"object"})";

			std::ignore = registry.add( shell );

			loop = std::make_unique< agent_loop >( deps );
		}

		auto connect( ) -> void {
			loop->register_handler( "echo", []( std::string_view args ) -> result< std::string > {
				return std::string{ args };
			} );

			loop->register_handler( "bash", []( std::string_view args ) -> result< std::string > {
				auto command = std::string{ args };

				if ( command.find( "crash" ) != std::string::npos ) {
					return std::unexpected( fail( errc::tool_failed, "spawn failed" ) );
				}

				const auto exit_code = command.find( "pass" ) != std::string::npos ? 0 : 1;

				auto out = std::string{ "{\"ok\":true,\"exit_code\":" };
				out += std::to_string( exit_code );
				out += "}";

				return out;
			} );
		}
	};

	inline auto text_response( const std::string_view text ) -> scripted_client::response {
		auto value = scripted_client::response{ };
		value.text = std::string{ text };

		return value;
	}

	inline auto call_response( const std::string_view name, const std::string_view args )
		-> scripted_client::response {
		auto value = scripted_client::response{ };
		value.calls.push_back(
			mcode::tool_call{ std::string{ }, std::string{ name }, std::string{ args } } );

		return value;
	}


} // namespace loop_test
