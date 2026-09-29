#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>

#include "mcode/model/client.hxx"
#include "mcode/model/types.hxx"
#include "mcode/net/http_client.hxx"

namespace mcode::model {

	// The canonical failure classes, from the model layer's taxonomy. `errc`
	// maps onto them, but the retry decision needs the class itself: a 429 is
	// two different failures depending on the body, and both are `protocol` at
	// the transport.
	enum class failure_class {
		// 429 non-quota, 5xx, 529, timeouts, network errors. Retry with backoff.
		transient,

		// 401/403/402/413, quota codes, invalid schema. Never retried.
		fatal,

		// The body names context length or too many tokens. The caller trims or
		// compacts; the client surfaces it and may retry once.
		context_overflow,

		// A refusal. Never retried; surfaced as its own outcome.
		content_filter,

		// The stream was consumed past its first event. Never retried, because
		// the sink has already seen tokens and the caller has appended them to
		// history.
		partial_stream,
	};

	[[nodiscard]] auto to_string( const failure_class value ) noexcept -> std::string_view;

	// Classifies a non-2xx response. The body is the only place the 429 split
	// lives: `rate_limit_exceeded` is transient, `insufficient_quota` and the
	// spend-cap and credit codes are billing and never succeed on retry.
	[[nodiscard]] auto classify_failure( const int status, const std::string_view body ) -> failure_class;

	// Backoff delay for one attempt, full jitter: a uniform draw from
	// [0, min(cap, base * 2^attempt)]. The RNG is injectable so a test is
	// deterministic.
	[[nodiscard]] auto backoff_delay( const unsigned attempt, std::mt19937_64& rng )
		-> std::chrono::milliseconds;

	// The `Retry-After` header, parsed as seconds. Absent when the header is
	// missing or not a number; an HTTP-date form is not supported, because no
	// model provider in the taxonomy uses it.
	[[nodiscard]] auto retry_after_seconds( const net::http_failure& failure )
		-> std::optional< std::int64_t >;

	// The streaming client over the net transport. Owns the retry policy: a
	// transport-level failure is retried here, with full-jitter backoff capped
	// at thirty seconds and at most five attempts, honouring `Retry-After`.
	//
	// A partially-consumed stream is NEVER retried -- the sink has already seen
	// tokens, the caller has appended them to history, and replaying the turn
	// would duplicate them. The partial text is surfaced and the caller
	// re-issues the turn.
	class http_model_client final : public model_client {
	public:
		// The clock and the sleeper are injectable so a test drives the retry
		// loop deterministically: no test waits a real second, and no test
		// depends on wall-clock timing.
		using sleep_function = std::function< void( std::chrono::milliseconds ) >;
		using rng_function = std::function< std::uint64_t( ) >;

		// Named `client_options`, not `options`: a nested type called `options`
		// collides with the conventional parameter name, and GCC rejects the
		// parameter as shadowing the member type (-Wshadow). Clang does not
		// implement that warning at all, so it passed the local gate and failed
		// the GCC leg of CI. The other request structs follow this convention
		// already (`process_options`, `loader_options`).
		struct client_options {
			sleep_function sleep;
			rng_function random;
		};

		explicit http_model_client( net::http_client& transport, client_options options = { } );

		auto stream( const stream_request& request, const event_sink& sink ) -> status override;

		// Everything the stream reported, folded through `usage::add`'s
		// max-of-observed rule. The loop charges the budget from this.
		[[nodiscard]] auto accumulated_usage( ) const noexcept -> const usage& { return usage_; }

	private:
		auto attempt( const stream_request& request, const event_sink& sink,
			failure_class& outcome, net::http_failure& failure ) -> status;

		net::http_client* transport_;
		client_options options_;
		usage usage_;
	};

}
