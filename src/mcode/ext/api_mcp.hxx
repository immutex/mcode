#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::ext {

	// Where extension-declared MCP servers go. The surface holds one per VM; the
	// transport reads the accumulated set after loading, exactly as it reads the
	// config-parsed set, so both routes funnel into one consumer.
	class mcp_server_store {
	public:
		// Adds one server. A name already claimed -- by config or by another
		// extension -- is an error rather than a silent shadow, because two
		// servers under one name would make `mcp__<server>__<tool>` ambiguous.
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
