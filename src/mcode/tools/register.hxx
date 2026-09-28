#pragma once

#include <string>

#include "mcode/core/registry.hxx"
#include "mcode/tools/context.hxx"

namespace mcode::tools {

	// Validates a JSON-Schema subset: an object with a `type` of `object`, a
	// `properties` map, and a `required` array of strings naming declared
	// properties. Schemas from extensions and MCP are untrusted input; a malformed
	// one is refused at registration rather than passed to the model.
	[[nodiscard]] auto validate_schema( std::string_view schema_json ) -> status;

	// Registers the eight core tools: schema into the registry, handler into the
	// sink. The registry entry and the handler are added in one call so the two
	// lists cannot drift.
	//
	// `sink` is the dependency-inversion seam: the agent loop implements it (its
	// `register_handler` already matches), and tests pass a stub. This header
	// never includes agent/loop.hxx -- layers point down only.
	[[nodiscard]] auto register_core_tools( tool_registry& registry, tool_handler_sink& sink,
		const tool_context& context ) -> status;

}
