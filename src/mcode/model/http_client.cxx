#include "mcode/model/http_client.hxx"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/render.hxx"
#include "mcode/net/sse.hxx"
#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

		// Retry policy, from the model layer: exponential backoff with full
		// jitter, capped, at most five attempts per request.
		inline constexpr unsigned MAX_ATTEMPTS = 5;
		inline constexpr std::chrono::milliseconds BACKOFF_BASE{ 500 };
		inline constexpr std::chrono::milliseconds BACKOFF_CAP{ 30'000 };
		inline constexpr int RATE_LIMIT_STATUS = 429;
		inline constexpr unsigned MAX_BACKOFF_SHIFT = 16;
		inline constexpr std::int64_t MILLISECONDS_PER_SECOND = 1000;

		// Status codes that are fatal regardless of the body. 413 is fatal, not
		// context overflow: the doc is explicit that an oversized request is a
		// client bug to fix, not a context to trim.
		[[nodiscard]] auto is_fatal_status( const int status ) noexcept -> bool {
			switch ( status ) {
				case 401:
				case 402:
				case 403:
				case 413:
					return true;

				default:
					return false;
			}
		}

		[[nodiscard]] auto is_transient_status( const int status ) noexcept -> bool {
			switch ( status ) {
				case RATE_LIMIT_STATUS:
				case 500:
				case 502:
				case 503:
				case 504:
				case 529:
					return true;

				default:
					return false;
			}
		}

		// Body markers that name context overflow. Matched lowercased.
		[[nodiscard]] auto body_names_context_overflow( const std::string_view body ) -> bool {
			static constexpr std::string_view MARKERS[] = {
				"context length",
				"maximum context length",
				"too many tokens",
				"prompt is too long",
				"input length exceeds",
			};

			for ( const auto marker : MARKERS ) {
				if ( body.find( marker ) != std::string_view::npos ) {
					return true;
				}
			}

			return false;
		}

		// The 429 split. A rate limit clears on retry; a quota or spend-cap code
		// is billing and never will, so retrying burns the user's time on a
		// request that cannot succeed.
		[[nodiscard]] auto body_names_quota( const std::string_view body ) -> bool {
			static constexpr std::string_view MARKERS[] = {
				"insufficient_quota",
				"spend_limit_exceeded",
				"credit_balance_exhausted",
				"billing",
			};

			for ( const auto marker : MARKERS ) {
				if ( body.find( marker ) != std::string_view::npos ) {
					return true;
				}
			}

			return false;
		}

		// A refusal is its own outcome, distinct from a hard failure: the model
		// answered, with something the harness must surface rather than retry.
		[[nodiscard]] auto body_names_content_filter( const std::string_view body ) -> bool {
			static constexpr std::string_view MARKERS[] = {
				"content_filter",
				"content_policy_violation",
			};

			for ( const auto marker : MARKERS ) {
				if ( body.find( marker ) != std::string_view::npos ) {
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] auto lower( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			std::transform( out.begin( ), out.end( ), out.begin( ),
				[]( const unsigned char byte ) { return static_cast< char >( std::tolower( byte ) ); } );

			return out;
		}

		// The sleep the client uses when the caller did not inject one. A real
		// sleep is the production behaviour; a test always injects.
		auto thread_sleep( const std::chrono::milliseconds duration ) -> void {
			std::this_thread::sleep_for( duration );
		}

		// The default RNG. Seed quality does not matter for jitter; the value
		// only needs to spread the retries.
		auto default_random( ) -> std::uint64_t {
			auto generator = std::random_device{ };

			return ( static_cast< std::uint64_t >( generator( ) ) << 32 ) | generator( );
		}

		// Builds the transport request from a descriptor and a rendered body.
		// The auth header and the descriptor's extra headers are merged here, so
		// the transport sees one flat header set.
		[[nodiscard]] auto build_http_request( const stream_request& request,
			const std::string& body ) -> result< net::http_request > {
			auto header_value = auth_header_value( request.provider.auth, request.api_key );

			if ( !header_value ) {
				return std::unexpected( header_value.error( ) );
			}

			auto out = net::http_request{ };
			out.url = request.provider.endpoint;
			out.body = body;

			if ( !header_value->empty( ) ) {
				out.headers[ request.provider.auth.header ] = *header_value;
			}

			if ( !request.provider.extra_headers_json.empty( ) ) {
				auto extra = json::document::parse( request.provider.extra_headers_json );

				if ( !extra ) {
					return std::unexpected( fail( errc::config,
						"provider '" + request.provider.name + "' has malformed extra headers: " +
						extra.error( ).msg ) );
				}

				for ( const auto& key : extra->keys_at( "" ) ) {
					auto value = extra->pointer_string( "/" + key );

					if ( !value ) {
						return std::unexpected( fail( errc::config,
							"provider '" + request.provider.name + "' has a non-string extra header '" +
							key + "'" ) );
					}

					out.headers[ key ] = *value;
				}
			}

			return out;
		}

	}

	auto to_string( const failure_class value ) noexcept -> std::string_view {
		switch ( value ) {
			case failure_class::transient: return "transient";
			case failure_class::fatal: return "fatal";
			case failure_class::context_overflow: return "context_overflow";
			case failure_class::content_filter: return "content_filter";
			case failure_class::partial_stream: return "partial_stream";
		}

		return "unknown";
	}

	auto classify_failure( const int status, const std::string_view raw_body ) -> failure_class {
		const auto body = lower( raw_body );

		if ( body_names_content_filter( body ) ) {
			return failure_class::content_filter;
		}

		if ( is_fatal_status( status ) ) {
			return failure_class::fatal;
		}

		if ( status == RATE_LIMIT_STATUS ) {
			return body_names_quota( body ) ? failure_class::fatal : failure_class::transient;
		}

		if ( body_names_context_overflow( body ) ) {
			return failure_class::context_overflow;
		}

		if ( is_transient_status( status ) ) {
			return failure_class::transient;
		}

		return failure_class::fatal;
	}

	auto backoff_delay( const unsigned attempt, std::mt19937_64& rng ) -> std::chrono::milliseconds {
		const auto shift = std::min< unsigned >( attempt, MAX_BACKOFF_SHIFT );
		const auto exponential = std::chrono::milliseconds{
			BACKOFF_BASE * ( 1ull << shift ) };
		const auto bounded = std::min( exponential, BACKOFF_CAP );

		auto spread = std::uniform_int_distribution< long long >{ 0, bounded.count( ) };

		return std::chrono::milliseconds{ spread( rng ) };
	}

	auto retry_after_seconds( const net::http_failure& failure ) -> std::optional< std::int64_t > {
		for ( const auto& [ name, text ] : failure.headers ) {
			if ( lower( name ) != "retry-after" ) {
				continue;
			}

			auto value = std::int64_t{ 0 };
			const auto* first = text.data( );
			const auto* last = first + text.size( );
			const auto parsed = std::from_chars( first, last, value );

			if ( parsed.ec == std::errc{ } && parsed.ptr == last && value >= 0 ) {
				return value;
			}

			return std::nullopt;
		}

		return std::nullopt;
	}

	http_model_client::http_model_client( net::http_client& transport, client_options options )
		: transport_( &transport ), options_( std::move( options ) ) {
		if ( !options_.sleep ) {
			options_.sleep = thread_sleep;
		}

		if ( !options_.random ) {
			options_.random = default_random;
		}
	}

	auto http_model_client::attempt( const stream_request& request, const event_sink& sink,
		failure_class& outcome, net::http_failure& failure ) -> status {
		auto rendered = render_request( request );

		if ( !rendered ) {
			return std::unexpected( rendered.error( ) );
		}

		auto http = build_http_request( request, *rendered );

		if ( !http ) {
			return std::unexpected( http.error( ) );
		}

		auto applier = delta_applier{ request.provider };
		auto saw_event = false;

		const auto sent = transport_->stream_sse( *http,
			[ & ]( net::sse_event&& event ) {
				auto produced = applier.feed( event.event, event.data );

				if ( !produced ) {
					throw std::runtime_error{ produced.error( ).msg };
				}

				for ( const auto& produced_event : *produced ) {
					saw_event = true;
					sink( produced_event );
				}
			},
			&failure );

		// The stream callback cannot return a failure, so a malformed payload
		// surfaces here. Whatever reached the sink already did, which is why the
		// class below is partial_stream rather than a clean error.
		if ( !sent ) {
			if ( saw_event ) {
				outcome = failure_class::partial_stream;

				return std::unexpected( sent.error( ) );
			}

			if ( failure.status == 0 ) {
				// No response at all: DNS, refused connection, timeout. All
				// transient by the taxonomy.
				outcome = failure_class::transient;

				return std::unexpected( sent.error( ) );
			}

			outcome = classify_failure( failure.status, failure.body );

			return std::unexpected( fail( errc::protocol,
				"HTTP " + std::to_string( failure.status ) + ": " + sent.error( ).msg ) );
		}

		for ( auto& event : applier.finish( ) ) {
			sink( event );
		}

		if ( !applier.saw_terminal_event( ) ) {
			outcome = saw_event ? failure_class::partial_stream : failure_class::transient;

			return std::unexpected( fail( errc::protocol,
				"stream ended without a terminal event; the turn was truncated" ) );
		}

		usage_.input = std::max( usage_.input, applier.accumulated_usage( ).input );
		usage_.output = std::max( usage_.output, applier.accumulated_usage( ).output );
		usage_.cached_read = std::max( usage_.cached_read, applier.accumulated_usage( ).cached_read );
		usage_.cache_write = std::max( usage_.cache_write, applier.accumulated_usage( ).cache_write );
		usage_.reasoning = std::max( usage_.reasoning, applier.accumulated_usage( ).reasoning );

		return { };
	}

	auto http_model_client::stream( const stream_request& request, const event_sink& sink )
		-> status {
		if ( !is_renderable_shape( request.provider ) ) {
			return std::unexpected( fail( errc::unsupported,
				"provider '" + request.provider.name +
				"' uses a request shape this build cannot render; only the chat-completions shape is supported" ) );
		}

		auto rng = std::mt19937_64{ options_.random ? options_.random( ) : 0u };

		for ( unsigned attempt_index = 0; attempt_index < MAX_ATTEMPTS; ++attempt_index ) {
			auto outcome = failure_class::fatal;
			auto failure = net::http_failure{ };
			auto result = attempt( request, sink, outcome, failure );

			if ( result ) {
				return result;
			}

			const auto error_message = result.error( ).msg;

			switch ( outcome ) {
				case failure_class::transient:
				case failure_class::context_overflow:
					break;

				case failure_class::fatal:
				case failure_class::content_filter:
				case failure_class::partial_stream:
					return std::unexpected( fail( result.error( ).code, error_message ) );
			}

			// The last attempt does not sleep: nothing follows it, so the delay
			// would be pure latency before the caller sees the error.
			if ( attempt_index + 1 >= MAX_ATTEMPTS ) {
				return std::unexpected( fail( result.error( ).code, error_message ) );
			}

			auto delay = std::chrono::milliseconds{ 0 };

			if ( failure.status != 0 ) {
				if ( const auto seconds = retry_after_seconds( failure ) ) {
					delay = std::chrono::milliseconds{ *seconds * MILLISECONDS_PER_SECOND };
				}
			}

			if ( delay.count( ) <= 0 ) {
				delay = backoff_delay( attempt_index, rng );
			}

			options_.sleep( delay );
		}

		return std::unexpected( fail( errc::protocol, "retry attempts exhausted" ) );
	}

}
