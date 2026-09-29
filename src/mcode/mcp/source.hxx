#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mcode/core/registry.hxx"
#include "mcode/mcp/supervisor.hxx"
#include "mcode/mcp/constants.hxx"

namespace mcode::mcp {

	// Registers a server's tools into the shared registry.
	//
	// Naming: `mcp__<server>__<tool>`, `tool_source::mcp`, `owner = "mcp:<server>"`.
	// The class is `tool_class::mcp` -- assigned by the registry path here, never
	// by the tool. `readOnlyHint` is a server claim; the spec requires clients to
	// treat annotations as untrusted, and a server must not be able to mark
	// itself read-only to skip the approval path.
	class source {
	public:
		explicit source( tool_registry& registry ) : registry_( &registry ) { }

		// Registers one tool. The description is wrapped in untrusted-data
		// delimiters before it reaches the model: a tool description is
		// attacker-controlled input, never interpolated as instructions.
		auto register_tool( const std::string& server_name, const server_tool& tool )
			-> result< void >;

		// Registers every tool of a (re)connected server. On a restart this is
		// the re-registration path: the tools come back, they are not left
		// deregistered.
		auto register_server( const std::string& server_name,
			const std::vector< server_tool >& tools ) -> result< void >;

		// Removes one server's tools. Called when a server exhausts its restart
		// budget, or on shutdown. Removing something already gone is a no-op.
		auto unregister_server( const std::string& server_name ) -> std::size_t;

	private:
		tool_registry* registry_ = nullptr;
	};

	// The registry-side name for one server tool, `mcp__<server>__<tool>`.
	// Shared so registration and handler wiring cannot drift.
	[[nodiscard]] auto qualified_tool_name( const std::string& server_name,
		const std::string& tool_name ) -> std::string;

}
