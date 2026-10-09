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

		inline constexpr unsigned MAX_ATTEMPTS = 5;
		inline constexpr std::chrono::milliseconds BACKOFF_BASE{ 500 };
		inline constexpr std::chrono::milliseconds BACKOFF_CAP{ 30'000 };
		inline constexpr int RATE_LIMIT_STATUS = 429;
		inline constexpr unsigned MAX_BACKOFF_SHIFT = 16;
		inline constexpr std::int64_t MILLISECONDS_PER_SECOND = 1000;

		// The longest wait a `Retry-After` header may impose. Two reasons: the
		// header is the server's own and unbounded, so `seconds *
		// MILLISECONDS_PER_SECOND` overflows to a negative delay past ~9.2e15 and
		// silently falls through to the short backoff; and an hour is already far
		// past any wait this harness should absorb on the user's behalf.
		inline constexpr std::int64_t MAX_RETRY_AFTER_SECONDS = 3'600;

		// Bounds the retries a request-field downgrade may add, so an unhelpful body cannot
		// loop: three fields can be dropped, so three retries is the most it can produce.
		inline constexpr unsigned MAX_FEATURE_DOWNGRADES = 3;

		// 413 is fatal, not context overflow: an oversized request is a client bug to fix.
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

		// a rate limit clears on retry; a quota or spend-cap code never will.
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
				[]( const unsigned char byte ) {
					return static_cast< char >( std::tolower( byte ) );
				} );

			return out;
		}

		auto thread_sleep( const std::chrono::milliseconds duration ) -> void {
			std::this_thread::sleep_for( duration );
		}

		auto default_random( ) -> std::uint64_t {
			auto generator = std::random_device{ };

			return ( static_cast< std::uint64_t >( generator( ) ) << 32 ) | generator( );
		}

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
							"provider '" + request.provider.name +
							"' has a non-string extra header '" + key + "'" ) );
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

	auto backoff_delay( const unsigned attempt, std::mt19937_64& rng )
		-> std::chrono::milliseconds {
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

	// A 400 that names a request field means the gateway does not implement that field. The
	// body is scanned for the field names this build can send, so an unsupported optimisation
	// costs the optimisation rather than the run.
	auto detect_feature_downgrade( const std::string_view raw_body ) -> feature_downgrade {
		static constexpr std::string_view CANDIDATES[] = {
			"parallel_tool_calls",
			"prefill",
			"strict",
			"tool_choice",
		};

		const auto body = lower( raw_body );

		for ( const auto candidate : CANDIDATES ) {
			if ( body.find( candidate ) != std::string::npos ) {
				return feature_downgrade{ .field = std::string{ candidate }, .matched = true };
			}
		}

		return feature_downgrade{ };
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

		// the stream callback cannot return a failure, so a malformed payload surfaces here.
		if ( !sent ) {
			if ( saw_event ) {
				outcome = failure_class::partial_stream;

				return std::unexpected( sent.error( ) );
			}

			if ( failure.status == 0 ) {
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
		usage_.cached_read =
			std::max( usage_.cached_read, applier.accumulated_usage( ).cached_read );
		usage_.cache_write =
			std::max( usage_.cache_write, applier.accumulated_usage( ).cache_write );
		usage_.reasoning = std::max( usage_.reasoning, applier.accumulated_usage( ).reasoning );

		return { };
	}

	auto http_model_client::stream( const stream_request& request, const event_sink& sink )
		-> status {
		if ( !is_renderable_shape( request.provider ) ) {
			return std::unexpected( fail( errc::unsupported,
				"provider '" + request.provider.name + "' uses a request shape this build "
				"cannot render; only the chat-completions shape is supported" ) );
		}

		auto rng = std::mt19937_64{ options_.random ? options_.random( ) : 0u };

		// A downgrade rewrites the request, so the retry loop works on a copy the caller
		// never sees. Each field is dropped at most once per stream, which bounds the loop.
		auto active = request;
		auto downgrades_left = MAX_FEATURE_DOWNGRADES;

		for ( unsigned attempt_index = 0; attempt_index < MAX_ATTEMPTS; ++attempt_index ) {
			auto outcome = failure_class::fatal;
			auto failure = net::http_failure{ };
			auto result = attempt( active, sink, outcome, failure );

			if ( result ) {
				return result;
			}

			const auto error_message = result.error( ).msg;

			switch ( outcome ) {
				case failure_class::transient:
				case failure_class::context_overflow:
					break;

				case failure_class::fatal: {
					// A 400 naming a request field is the gateway refusing an optimisation it
					// does not implement, not a malformed request. Dropping the field and
					// retrying immediately is cheaper than failing a run that would work.
					if ( failure.status != 400 ) {
						return std::unexpected( fail( result.error( ).code, error_message ) );
					}

					const auto downgrade = detect_feature_downgrade( failure.body );

					if ( !downgrade.matched ) {
						return std::unexpected( fail( result.error( ).code, error_message ) );
					}

					auto applied = false;

					if ( downgrade.field == "parallel_tool_calls" &&
						active.request.parallel_tool_calls.has_value( ) ) {
						active.request.parallel_tool_calls.reset( );
						applied = true;
					} else if ( downgrade.field == "prefill" &&
						!active.request.prefill.empty( ) ) {
						active.request.prefill.clear( );
						applied = true;
					} else if ( downgrade.field == "strict" && active.request.strict_tools ) {
						active.request.strict_tools = false;
						applied = true;
					} else if ( downgrade.field == "tool_choice" &&
						active.request.choice.kind != tool_choice_kind::automatic ) {
						active.request.choice = tool_choice{ };
						applied = true;
					}

					if ( !applied || downgrades_left == 0 ) {
						return std::unexpected( fail( result.error( ).code, error_message ) );
					}

					--downgrades_left;

					// The retry does not consume an attempt: the request that failed was never
					// one the gateway could have served.
					--attempt_index;

					continue;
				}

				case failure_class::content_filter:
				case failure_class::partial_stream:
					return std::unexpected( fail( result.error( ).code, error_message ) );
			}

			if ( attempt_index + 1 >= MAX_ATTEMPTS ) {
				return std::unexpected( fail( result.error( ).code, error_message ) );
			}

			auto delay = std::chrono::milliseconds{ 0 };

			if ( failure.status != 0 ) {
				if ( const auto seconds = retry_after_seconds( failure ) ) {
					// Clamped twice: the server's value is unbounded, and
					// `seconds * MILLISECONDS_PER_SECOND` overflows to a negative
					// delay for anything past ~9.2e15 -- which then falls through to
					// the backoff, so a gateway asking for a long wait would get a
					// short one. The cap is the longest wait this loop will honour
					// regardless, so a hostile or misconfigured header cannot stall
					// the run indefinitely either.
					const auto bounded = std::min< std::int64_t >( *seconds,
						MAX_RETRY_AFTER_SECONDS );

					delay = std::chrono::milliseconds{ bounded * MILLISECONDS_PER_SECOND };
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
