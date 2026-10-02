#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/net/sse.hxx"

namespace mcode::net {

	inline constexpr std::int64_t DEFAULT_TIMEOUT_SECONDS = 60;
	inline constexpr std::uint64_t DEFAULT_MAX_RESPONSE_BYTES = 32ull * 1024ull * 1024ull;

	struct http_request {
		std::string url;
		std::string method = "POST";
		std::string body;
		std::map< std::string, std::string, std::less<> > headers;
		std::int64_t timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
	};

	struct http_response {
		int status = 0;
		std::string body;
		std::map< std::string, std::string, std::less<> > headers;
	};

	struct url {
		std::string scheme;
		std::string host;
		std::string port;
		std::string target;
	};

	// The taxonomy lives in the body and headers, so the parts are handed back undecided.
	struct http_failure {
		int status = 0;
		std::string body;
		std::map< std::string, std::string, std::less<> > headers;
	};

	// Bounded so a gateway answering with HTML cannot exhaust memory on the failure path.
	inline constexpr std::uint64_t MAX_ERROR_BODY_BYTES = 64ull * 1024ull;

	[[nodiscard]] auto parse_url( const std::string_view text ) -> result< url >;

	class http_client {
	public:
		http_client( ) = default;
		~http_client( ) = default;

		http_client( const http_client& ) = delete;
		auto operator=( const http_client& ) -> http_client& = delete;

		[[nodiscard]] auto send( const http_request& request ) -> result< http_response >;

		// `failure` is filled only for a non-2xx response; other failures leave it untouched.
		[[nodiscard]] auto stream_sse( const http_request& request, sse_parser::event_callback on_event,
			http_failure* failure = nullptr ) -> status;

		auto set_max_response_bytes( const std::uint64_t bytes ) noexcept -> void {
			max_response_bytes_ = bytes;
		}

	private:
		std::uint64_t max_response_bytes_ = DEFAULT_MAX_RESPONSE_BYTES;
	};

}
