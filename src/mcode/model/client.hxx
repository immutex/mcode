#pragma once

#include <functional>
#include <optional>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	// Everything one streamed call needs, assembled by the caller.
	//
	// A struct rather than five positional parameters, and the group is the whole
	// input: there is no sixth thing a client has to be told. The provider travels
	// with the request because a descriptor is what says where the fields go.
	struct stream_request {
		chat_request request;
		provider_descriptor provider;

		// Already resolved by the caller. The client never reads the environment
		// or the config: that is the host's decision, and a client that resolved
		// its own credential could not be tested without one.
		std::string api_key;

		// What the model accepts, for the limit and cost checks the client makes
		// before it spends a request.
		capabilities caps;
	};

	// Receives one canonical event at a time, in order.
	//
	// A const reference and not a value: the events carry the tool-call fragments
	// the loop accumulates, and copying each one per token is the hot path.
	using event_sink = std::function< void( const chat_event& ) >;

	// A streaming completion client.
	//
	// Abstract so the loop can be driven by a scripted fake. The loop's
	// correctness is the state machine's, not the transport's, and a test that
	// needs a network to exercise it is a test that runs nowhere.
	class model_client {
	public:
		model_client( ) = default;
		virtual ~model_client( ) = default;

		model_client( const model_client& ) = delete;
		auto operator=( const model_client& ) -> model_client& = delete;
		model_client( model_client&& ) = delete;
		auto operator=( model_client&& ) -> model_client& = delete;

		// Streams one completion, emitting every event through `sink` before it
		// returns.
		//
		// A mid-stream failure is a FAILED call, not a short one: the caller must
		// be able to tell a truncated turn from a complete one, and a silent short
		// turn is how a half-finished edit becomes a "done".
		//
		// Retry lives here, because the policy is a property of the transport:
		// exponential backoff with full jitter, capped at 30 s, at most 5 attempts,
		// `Retry-After` honoured. A partially-consumed stream is NEVER retried --
		// the partial text is surfaced and the caller re-issues the turn.
		virtual auto stream( const stream_request& request, const event_sink& sink ) -> status = 0;
	};

	// Renders a request body from a descriptor, byte-stably.
	//
	// Byte-stable means: fixed key order, sorted tool schemas, no timestamps. The
	// provider's prompt cache hashes the prefix, so an unstable key order
	// invalidates it silently and costs full price on every turn.
	[[nodiscard]] auto render_request( const stream_request& request ) -> result< std::string >;

	// The auth header value for a descriptor, or empty when the source is `none`.
	//
	// `api_key` is passed in rather than looked up, so this stays a pure function
	// and the resolution policy lives in one place.
	[[nodiscard]] auto auth_header_value( const auth_spec& auth, std::string_view api_key )
		-> result< std::string >;

	// Resolves a descriptor's credential from the environment or the config.
	// Fails when the source is set but the value is missing -- a request sent
	// without its credential is a 401 the user has to decode.
	[[nodiscard]] auto resolve_api_key( const auth_spec& auth,
		const std::function< std::optional< std::string >( std::string_view ) >& from_config )
		-> result< std::string >;

}
