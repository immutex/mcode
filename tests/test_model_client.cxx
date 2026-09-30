#include <catch2/catch_test_macros.hpp>

#include "socket_test_helpers.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mcode/model/capabilities.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/toml.hxx"
#include "mcode/model/credentials.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"

#include <string_view>
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

	using mcode::test::INVALID_SOCKET_HANDLE;
	using mcode::test::close_socket;
	using mcode::test::host_to_network_long;
	using mcode::test::network_to_host_short;
	using mcode::test::receive_bytes;
	using mcode::test::send_bytes;
	using mcode::test::socket_handle;

	// A scripted loopback server: one response per accepted connection, in
	// order. The retry tests need a server that answers differently per
	// attempt, which a one-shot response cannot do. No external network.
	class scripted_server {
	public:
		explicit scripted_server( std::vector< std::string > responses )
			: responses_( std::move( responses ) ) {
		#if defined( _WIN32 )
			auto data = WSADATA{ };
			::WSAStartup( MAKEWORD( 2, 2 ), &data );
		#endif

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

			if ( ::getsockname( socket_, reinterpret_cast< sockaddr* >( &address ), &length ) != 0 ) {
				return;
			}

			port_ = network_to_host_short( address.sin_port );
			worker_ = std::thread{ [this] { serve( ); } };
		}

		~scripted_server( ) {
			stopped_ = true;

			// A blocked accept() never notices the flag, so the listening socket
			// is shut down first: a correct client that made fewer requests than
			// scripted must not hang the test, and a client that made MORE must
			// still terminate once the script is exhausted.
			shutdown_socket( socket_ );
			close_socket( socket_ );

			if ( worker_.joinable( ) ) {
				worker_.join( );
			}

		#if defined( _WIN32 )
			::WSACleanup( );
		#endif
		}

		scripted_server( const scripted_server& ) = delete;
		auto operator=( const scripted_server& ) -> scripted_server& = delete;

		[[nodiscard]] auto url( ) const -> std::string {
			return "http://127.0.0.1:" + std::to_string( port_ ) + "/v1/chat/completions";
		}

		[[nodiscard]] auto requests_seen( ) const noexcept -> std::size_t { return requests_; }

	private:
		static auto close_socket( const socket_handle handle ) -> void {
		#if defined( _WIN32 )
			::closesocket( handle );
		#else
			::close( handle );
		#endif
	}

		static auto shutdown_socket( const socket_handle handle ) -> void {
		#if defined( _WIN32 )
			::shutdown( handle, SD_BOTH );
		#else
			::shutdown( handle, SHUT_RDWR );
		#endif
	}

		// Serves one scripted response per accepted connection. The loop is
		// driven by the stop flag, not the response count: the test's point is
		// often that the client makes FEWER requests than scripted, and a loop
		// over the script would block in accept() forever exactly when the
		// implementation is correct. A client that makes more requests than
		// scripted gets the last response repeated, so it still terminates.
		auto serve( ) -> void {
			while ( !stopped_ ) {
				const auto accepted = ::accept( socket_, nullptr, nullptr );

				if ( accepted == INVALID_SOCKET_HANDLE ) {
					return;
				}

				auto scratch = std::array< char, 8192 >{ };
				receive_bytes( accepted, scratch );

				const auto index = std::min( requests_, responses_.size( ) - 1 );
				const auto& response = responses_[ index ];
				++requests_;

				(void)send_bytes( accepted, response );

				// Long enough for the client to drain both events of a multi-event
				// response before the socket goes away; too short and the tail of
				// the stream is lost to the reset.
				std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
				close_socket( accepted );
			}
		}

		std::vector< std::string > responses_;
		socket_handle socket_ = INVALID_SOCKET_HANDLE;
		std::uint16_t port_ = 0;
		std::size_t requests_ = 0;
		std::thread worker_;
		std::atomic< bool > stopped_{ false };
	};

	auto sse_response( const int status, const std::string& body ) -> std::string {
		auto head = std::string{ "HTTP/1.1 " + std::to_string( status ) + " reason\r\n"
			"Content-Type: text/event-stream\r\n" };

		if ( !body.empty( ) ) {
			head += "Content-Length: " + std::to_string( body.size( ) ) + "\r\n";
		}

		head += "Connection: close\r\n\r\n" + body;

		return head;
	}

	auto chat_completions_descriptor( const std::string& endpoint ) -> model::provider_descriptor {
		auto descriptor = model::provider_descriptor{ };
		descriptor.name = "openai-chat-completions";
		descriptor.endpoint = endpoint;
		descriptor.auth.from = model::auth_spec::source::none;
		descriptor.stream.text_delta = "/choices/0/delta/content";

		return descriptor;
	}

	auto make_request( const std::string& endpoint ) -> model::stream_request {
		auto request = model::chat_request{ };
		request.model = "test-model";

		auto user = model::message{ };
		user.speaker = model::role::user;

		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = "hi";

		user.blocks.push_back( std::move( block ) );
		request.messages.push_back( std::move( user ) );

		auto out = model::stream_request{ };
		out.request = std::move( request );
		out.provider = chat_completions_descriptor( endpoint );

		return out;
	}

	auto no_sleep( const std::chrono::milliseconds ) -> void { }

	// Deterministic jitter: always zero, so the retry loop's own sleeps are the
	// only variable and the tests observe delays through the injected sleeper.
	auto zero_random( ) -> std::uint64_t { return 0; }

	auto collect( std::vector< model::chat_event >& into ) -> model::event_sink {
		return [ &into ]( const model::chat_event& event ) { into.push_back( event ); };
	}

}

TEST_CASE( "credentials resolve per source", "[credentials]" ) {
	auto env_auth = model::auth_spec{ };
	env_auth.from = model::auth_spec::source::environment;
	env_auth.name = "MCODE_TEST_CREDENTIAL_THAT_DOES_NOT_EXIST";

	auto missing_env = model::resolve_api_key( env_auth, { } );
	REQUIRE_FALSE( static_cast< bool >( missing_env ) );
	REQUIRE( missing_env.error( ).msg.find( "MCODE_TEST_CREDENTIAL_THAT_DOES_NOT_EXIST" )
		!= std::string::npos );

	auto config_auth = model::auth_spec{ };
	config_auth.from = model::auth_spec::source::config;
	config_auth.name = "some.key";

	auto from_config = []( const std::string_view key ) -> std::optional< std::string > {
		if ( key == "some.key" ) {
			return std::string{ "secret" };
		}

		return std::nullopt;
	};

	auto found = model::resolve_api_key( config_auth, from_config );
	REQUIRE( static_cast< bool >( found ) );
	REQUIRE( *found == "secret" );

	auto missing_config_auth = model::auth_spec{ };
	missing_config_auth.from = model::auth_spec::source::config;
	missing_config_auth.name = "absent.key";

	auto missing_config = model::resolve_api_key( missing_config_auth, from_config );
	REQUIRE_FALSE( static_cast< bool >( missing_config ) );

	auto none_auth = model::auth_spec{ };
	auto bare = model::resolve_api_key( none_auth, { } );
	REQUIRE( static_cast< bool >( bare ) );
	REQUIRE( bare->empty( ) );
}

TEST_CASE( "the auth header carries the scheme, or the bare key without one", "[credentials]" ) {
	auto bearer = model::auth_spec{ };
	bearer.from = model::auth_spec::source::environment;
	bearer.scheme = "Bearer";

	auto value = model::auth_header_value( bearer, "key123" );
	REQUIRE( static_cast< bool >( value ) );
	REQUIRE( *value == "Bearer key123" );

	auto bare = model::auth_spec{ };
	bare.from = model::auth_spec::source::environment;
	bare.scheme = "";

	auto raw = model::auth_header_value( bare, "key123" );
	REQUIRE( static_cast< bool >( raw ) );
	REQUIRE( *raw == "key123" );

	auto none = model::auth_spec{ };
	auto empty = model::auth_header_value( none, "unused" );
	REQUIRE( static_cast< bool >( empty ) );
	REQUIRE( empty->empty( ) );

	auto missing = model::auth_header_value( bearer, "" );
	REQUIRE_FALSE( static_cast< bool >( missing ) );
}

TEST_CASE( "a transient failure is retried", "[retry]" ) {
	auto server = scripted_server{ {
		sse_response( 503, R"({"error":{"type":"server_error"}})" ),
		sse_response( 200, "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\ndata: [DONE]\n\n" ),
	} };

	auto transport = net::http_client{ };
	auto sleeps = std::vector< std::chrono::milliseconds >{ };

	auto options = model::http_model_client::client_options{ };
	options.sleep = [ &sleeps ]( const std::chrono::milliseconds duration ) { sleeps.push_back( duration ); };
	options.random = zero_random;

	auto client = model::http_model_client{ transport, options };

	auto events = std::vector< model::chat_event >{ };
	auto request = make_request( server.url( ) );

	const auto result = client.stream( request, collect( events ) );

	REQUIRE( static_cast< bool >( result ) );
	REQUIRE( server.requests_seen( ) == 2 );
	REQUIRE( sleeps.size( ) == 1 );

	auto text = std::string{ };

	for ( const auto& event : events ) {
		if ( event.type == model::chat_event::kind::text_delta ) {
			text += event.text;
		}
	}

	REQUIRE( text == "ok" );
}

TEST_CASE( "a fatal failure is not retried", "[retry]" ) {
	auto server = scripted_server{ {
		sse_response( 401, R"({"error":{"type":"authentication_error"}})" ),
	} };

	auto transport = net::http_client{ };

	auto options = model::http_model_client::client_options{ };
	options.sleep = no_sleep;
	options.random = zero_random;

	auto client = model::http_model_client{ transport, options };

	auto events = std::vector< model::chat_event >{ };
	auto request = make_request( server.url( ) );

	const auto result = client.stream( request, collect( events ) );

	REQUIRE_FALSE( static_cast< bool >( result ) );
	REQUIRE( server.requests_seen( ) == 1 );
	REQUIRE( result.error( ).msg.find( "401" ) != std::string::npos );
}

TEST_CASE( "a quota 429 is never retried but a rate-limit 429 is", "[retry]" ) {
	{
		auto server = scripted_server{ {
			sse_response( 429, R"({"error":{"type":"insufficient_quota"}})" ),
		} };

		auto transport = net::http_client{ };

		auto options = model::http_model_client::client_options{ };
		options.sleep = no_sleep;
		options.random = zero_random;

		auto client = model::http_model_client{ transport, options };

		auto request = make_request( server.url( ) );
		const auto result = client.stream( request, [ ]( const model::chat_event& ) { } );

		REQUIRE_FALSE( static_cast< bool >( result ) );
		REQUIRE( server.requests_seen( ) == 1 );
	}

	{
		auto server = scripted_server{ {
			sse_response( 429, R"({"error":{"type":"rate_limit_exceeded"}})" ),
			sse_response( 200, "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\ndata: [DONE]\n\n" ),
		} };

		auto transport = net::http_client{ };

		auto options = model::http_model_client::client_options{ };
		options.sleep = no_sleep;
		options.random = zero_random;

		auto client = model::http_model_client{ transport, options };

		auto events = std::vector< model::chat_event >{ };
		auto request = make_request( server.url( ) );

		const auto result = client.stream( request, collect( events ) );

		REQUIRE( static_cast< bool >( result ) );
		REQUIRE( server.requests_seen( ) == 2 );
	}
}

TEST_CASE( "a partial stream is never retried", "[retry]" ) {
	// Two events in one response, then the connection drops with no terminal
	// event. The sink has already seen text, so replaying would duplicate it
	// in history.
	auto server = scripted_server{ {
		sse_response( 200,
			"data: {\"choices\":[{\"delta\":{\"content\":\"partial \"}}]}\n\n"
			"data: {\"choices\":[{\"delta\":{\"content\":\"text\"}}]}\n\n" ),
	} };

	auto transport = net::http_client{ };

	auto options = model::http_model_client::client_options{ };
	options.sleep = no_sleep;
	options.random = zero_random;

	auto client = model::http_model_client{ transport, options };

	auto events = std::vector< model::chat_event >{ };
	auto request = make_request( server.url( ) );

	const auto result = client.stream( request, collect( events ) );

	REQUIRE_FALSE( static_cast< bool >( result ) );
	REQUIRE( server.requests_seen( ) == 1 );

	auto text = std::string{ };

	for ( const auto& event : events ) {
		if ( event.type == model::chat_event::kind::text_delta ) {
			text += event.text;
		}
	}

	// The partial text is surfaced, not swallowed.
	REQUIRE( text == "partial text" );
}

TEST_CASE( "retry honours Retry-After", "[retry]" ) {
	auto body = std::string{ R"({"error":{"type":"rate_limit_exceeded"}})" };
	auto head = std::string{ "HTTP/1.1 429 reason\r\n"
		"Content-Type: application/json\r\n"
		"Retry-After: 7\r\n"
		"Content-Length: " } + std::to_string( body.size( ) ) + "\r\n"
		"Connection: close\r\n\r\n" + body;

	auto server = scripted_server{ {
		head,
		sse_response( 200, "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\ndata: [DONE]\n\n" ),
	} };

	auto transport = net::http_client{ };

	auto options = model::http_model_client::client_options{ };
	options.sleep = no_sleep;
	options.random = zero_random;

	auto client = model::http_model_client{ transport, options };

	auto request = make_request( server.url( ) );
	const auto result = client.stream( request, [ ]( const model::chat_event& ) { } );

	REQUIRE( static_cast< bool >( result ) );
	REQUIRE( server.requests_seen( ) == 2 );
}

TEST_CASE( "classification maps the taxonomy", "[retry]" ) {
	using model::failure_class;

	REQUIRE( model::classify_failure( 429, R"({"error":{"type":"rate_limit_exceeded"}})" )
		== failure_class::transient );
	REQUIRE( model::classify_failure( 429, R"({"error":{"type":"insufficient_quota"}})" )
		== failure_class::fatal );
	REQUIRE( model::classify_failure( 429, R"({"error":{"code":"insufficient_quota"}})" )
		== failure_class::fatal );
	REQUIRE( model::classify_failure( 500, "" ) == failure_class::transient );
	REQUIRE( model::classify_failure( 529, "" ) == failure_class::transient );
	REQUIRE( model::classify_failure( 401, "" ) == failure_class::fatal );
	REQUIRE( model::classify_failure( 403, "" ) == failure_class::fatal );
	REQUIRE( model::classify_failure( 402, "" ) == failure_class::fatal );
	REQUIRE( model::classify_failure( 413, "" ) == failure_class::fatal );
	REQUIRE( model::classify_failure( 400,
		R"({"error":{"message":"This model's maximum context length is 8192 tokens"}})" )
		== failure_class::context_overflow );
	REQUIRE( model::classify_failure( 400, R"({"error":{"type":"content_policy_violation"}})" )
		== failure_class::content_filter );
	REQUIRE( model::classify_failure( 400, R"({"error":{"type":"invalid_request_error"}})" )
		== failure_class::fatal );
}

TEST_CASE( "usage folds by max across events", "[usage]" ) {
	auto client_usage = model::usage{ };

	auto first = model::chat_event{ };
	first.type = model::chat_event::kind::usage;
	first.input_tokens = 100;
	first.output_tokens = 10;
	client_usage.add( first );

	auto final_event = model::chat_event{ };
	final_event.type = model::chat_event::kind::usage;
	final_event.input_tokens = 90;
	final_event.output_tokens = 50;
	client_usage.add( final_event );

	REQUIRE( client_usage.input == 100 );
	REQUIRE( client_usage.output == 50 );
}

TEST_CASE( "a config entry prices a model the table does not carry", "[capabilities]" ) {
	// The gateway case: the compiled-in table cannot know every id a proxy
	// serves, so the user prices theirs. Absent from both sources still fails
	// closed -- the section below is what makes the id known.
	//
	// The id is quoted because it carries `/` and `.`; a quoted segment is taken
	// verbatim, so the entry key is the id as written, with no mangling.
	REQUIRE_FALSE( model::resolve_capabilities( "cb/gpt-5.6-sol", nullptr ).has_value( ) );

	auto parsed = toml::parse( R"(
[models."cb/gpt-5.6-sol"]
caching = "implicit"
context_window = 400000
max_output_tokens = 128000
price_input = 1.25
price_cached_read = 0.125
price_output = 10.0
supports_thinking = true
)" );

	REQUIRE( static_cast< bool >( parsed ) );

	auto layers = std::vector< config::layer >{ };
	layers.push_back( { .level = config::scope::user, .origin = { }, .values = std::move( *parsed ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	const auto caps = model::resolve_capabilities( "cb/gpt-5.6-sol", &*merged );
	REQUIRE( caps.has_value( ) );

	if ( caps ) {
		REQUIRE( caps->model == "cb/gpt-5.6-sol" );
		REQUIRE( caps->price_input == 1.25 );
		REQUIRE( caps->price_output == 10.0 );
		REQUIRE( caps->context_window == 400'000 );
		REQUIRE( caps->max_output_tokens == 128'000 );
		REQUIRE( caps->caching == model::cache_mode::implicit );
		REQUIRE( caps->supports_thinking );

		// Not set by the entry, so the struct default stands rather than a zero.
		REQUIRE( caps->supports_tool_calls );
	}

	// An entry that names no price is refused: it would price every turn at
	// zero, which is the failure the table exists to prevent.
	auto unpriced = toml::parse( R"(
[models.cheap]
supports_thinking = true
)" );
	REQUIRE( static_cast< bool >( unpriced ) );

	auto unpriced_layers = std::vector< config::layer >{ };
	unpriced_layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *unpriced ) } );

	auto unpriced_merged = config::merged_config::merge( std::move( unpriced_layers ) );
	REQUIRE( static_cast< bool >( unpriced_merged ) );
	REQUIRE_FALSE( model::resolve_capabilities( "cheap", &*unpriced_merged ).has_value( ) );

	// An integer price is the same number as a float one.
	auto integral = toml::parse( R"(
[models.flat]
price_input = 2
price_output = 4
)" );
	REQUIRE( static_cast< bool >( integral ) );

	auto integral_layers = std::vector< config::layer >{ };
	integral_layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *integral ) } );

	auto integral_merged = config::merged_config::merge( std::move( integral_layers ) );
	REQUIRE( static_cast< bool >( integral_merged ) );

	const auto flat = model::resolve_capabilities( "flat", &*integral_merged );
	REQUIRE( flat.has_value( ) );

	if ( flat ) {
		REQUIRE( flat->price_input == 2.0 );
		REQUIRE( flat->price_output == 4.0 );
	}
}

TEST_CASE( "an unknown model id does not price as free", "[capabilities]" ) {
	REQUIRE_FALSE( model::lookup_capabilities( "totally-made-up-model" ).has_value( ) );
	REQUIRE_FALSE( model::lookup_capabilities( "" ).has_value( ) );

	const auto known = model::lookup_capabilities( "claude-sonnet-4-5" );
	REQUIRE( known.has_value( ) );

	if ( known ) {
		REQUIRE( known->price_input == 3.0 );
		REQUIRE( known->price_output == 15.0 );
		REQUIRE( known->caching == model::cache_mode::explicit_markers );
		REQUIRE( known->context_window == 200'000 );
	}
}
