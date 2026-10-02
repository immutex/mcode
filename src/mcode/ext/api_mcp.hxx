#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::ext {

	// both the declaration route and the config route funnel into one consumer.
	class mcp_server_store {
	public:
		// a name already claimed by config or another extension is an error, not a shadow.
		auto add( mcode::mcp::server_config server ) -> status;

		[[nodiscard]] auto all( ) const noexcept -> const std::vector< mcode::mcp::server_config >& {
			return servers_;
		}

		[[nodiscard]] auto find( const std::string_view name ) const
			-> const mcode::mcp::server_config*;

		auto clear( ) noexcept -> void {
			servers_.clear( );
		}

	private:
		std::vector< mcode::mcp::server_config > servers_;
	};

}
