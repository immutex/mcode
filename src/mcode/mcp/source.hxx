#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mcode/core/registry.hxx"
#include "mcode/mcp/supervisor.hxx"
#include "mcode/mcp/constants.hxx"

namespace mcode::mcp {

	// readOnlyHint is a server claim: it must not let a server skip the approval path
	class source {
	public:
		explicit source( tool_registry& registry ) : registry_( &registry ) { }

		// the description is wrapped in untrusted delimiters before it reaches the model
		auto register_tool( const std::string& server_name, const server_tool& tool )
			-> result< void >;

		// the restart path: tools come back, they are not left deregistered
		auto register_server( const std::string& server_name,
			const std::vector< server_tool >& tools ) -> result< void >;

		// removing something already gone is a no-op
		auto unregister_server( const std::string& server_name ) -> std::size_t;

	private:
		tool_registry* registry_ = nullptr;
	};

	// mcp__<server>__<tool>; shared so registration and handler wiring cannot drift
	[[nodiscard]] auto qualified_tool_name( const std::string& server_name,
		const std::string& tool_name ) -> std::string;

}
