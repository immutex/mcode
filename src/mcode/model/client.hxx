#pragma once

#include <functional>
#include <optional>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	struct stream_request {
		chat_request request;
		provider_descriptor provider;

		// already resolved by the caller; the client never reads the environment.
		std::string api_key;

		capabilities caps;
	};

	using event_sink = std::function< void( const chat_event& ) >;

	class model_client {
	public:
		model_client( ) = default;
		virtual ~model_client( ) = default;

		model_client( const model_client& ) = delete;
		auto operator=( const model_client& ) -> model_client& = delete;
		model_client( model_client&& ) = delete;
		auto operator=( model_client&& ) -> model_client& = delete;

		// a mid-stream failure is a failed call, not a short one; a partial stream is not retried.
		virtual auto stream( const stream_request& request, const event_sink& sink ) -> status = 0;
	};

	// byte-stable: the prompt cache hashes the prefix, so unstable key order costs full price.
	[[nodiscard]] auto render_request( const stream_request& request ) -> result< std::string >;

	[[nodiscard]] auto auth_header_value( const auth_spec& auth, std::string_view api_key )
		-> result< std::string >;

	[[nodiscard]] auto resolve_api_key( const auth_spec& auth,
		const std::function< std::optional< std::string >( std::string_view ) >& from_config )
		-> result< std::string >;

}
