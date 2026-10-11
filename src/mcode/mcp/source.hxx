#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mcode/core/registry.hxx"
#include "mcode/mcp/supervisor.hxx"
#include "mcode/mcp/constants.hxx"

namespace mcode::mcp {

	class source {
	public:
		explicit source( tool_registry& registry ) : registry_( &registry ) { }

		auto register_tool( const std::string& server_name, const server_tool& tool )
			-> result< void >;

		// the restart path: tools come back, they are not left deregistered
		// `with_resources` adds one synthetic entry: a server names its resources and
		// the client reads them by URI, so the model needs a way to list them. One
		// entry per server keeps the always-loaded schema flat however many there are.
		auto register_server( const std::string& server_name,
			const std::vector< server_tool >& tools, bool with_resources = false )
			-> result< void >;

		// removing something already gone is a no-op
		auto unregister_server( const std::string& server_name ) -> std::size_t;

	private:
		tool_registry* registry_ = nullptr;
	};

	// mcp__<server>__<tool>; shared so registration and handler wiring cannot drift
	[[nodiscard]] auto qualified_tool_name( const std::string& server_name,
		const std::string& tool_name ) -> std::string;

}
