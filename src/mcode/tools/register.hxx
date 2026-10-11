#pragma once

#include <string>

#include "mcode/core/registry.hxx"
#include "mcode/tools/context.hxx"

namespace mcode::tools {

	// refuses a malformed schema at registration: extension and MCP schemas are untrusted input
	[[nodiscard]] auto validate_schema( std::string_view schema_json ) -> status;

	[[nodiscard]] auto register_core_tools( tool_registry& registry, tool_handler_sink& sink,
		const tool_context& context ) -> status;

}
