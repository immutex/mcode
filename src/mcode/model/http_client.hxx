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

	// a 429 is two different failures by body, so the retry decision needs the class, not errc.
	enum class failure_class {
		transient,

		fatal,

		context_overflow,

		content_filter,

		partial_stream,
	};

	[[nodiscard]] auto to_string( const failure_class value ) noexcept -> std::string_view;

	[[nodiscard]] auto classify_failure( const int status, const std::string_view body ) -> failure_class;

	// full jitter: a uniform draw from [0, min(cap, base * 2^attempt)].
	[[nodiscard]] auto backoff_delay( const unsigned attempt, std::mt19937_64& rng )
		-> std::chrono::milliseconds;

	// seconds only; the HTTP-date form is not supported.
	[[nodiscard]] auto retry_after_seconds( const net::http_failure& failure )
		-> std::optional< std::int64_t >;

	// A 400 whose body names a request field means the gateway does not implement that field,
	// not that the request is malformed. The field is switched off for the rest of the session
	// and the request retried, because failing the whole run over an unsupported optimisation
	// is worse than running without it.
	struct feature_downgrade {
		std::string field;
		bool matched = false;
	};

	[[nodiscard]] auto detect_feature_downgrade( std::string_view raw_body )
		-> feature_downgrade;

	class http_model_client final : public model_client {
	public:
		using sleep_function = std::function< void( std::chrono::milliseconds ) >;
		using rng_function = std::function< std::uint64_t( ) >;

		// not `options`: a nested type named `options` trips -Wshadow on the gcc leg.
		struct client_options {
			sleep_function sleep;
			rng_function random;
		};

		explicit http_model_client( net::http_client& transport, client_options options = { } );

		auto stream( const stream_request& request, const event_sink& sink ) -> status override;

		[[nodiscard]] auto accumulated_usage( ) const noexcept -> const usage& { return usage_; }

	private:
		auto attempt( const stream_request& request, const event_sink& sink,
			failure_class& outcome, net::http_failure& failure ) -> status;

		net::http_client* transport_;
		client_options options_;
		usage usage_;
	};

}
