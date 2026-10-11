#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"

namespace mcode {

	// Read-only is a capability set, not a prompt: the child's registry is filtered to
	// the read class, so a write call fails schema validation rather than reaching an
	// approval prompt. The child gets a fresh window, never the parent's context.
	struct subagent_request {
		std::string objective;
		std::string context;

		agent_loop::dependencies* parent = nullptr;

		std::vector< std::pair< std::string, agent_loop::tool_handler > > handlers;

		std::size_t max_steps = 24;
	};

	struct subagent_result {
		std::string summary;
		std::uint64_t steps = 0;
		bool truncated = false;
	};

	[[nodiscard]] auto subagent_tool_names( ) -> const std::vector< std::string_view >&;

	// Fails rather than skipping a tool it could not copy.
	[[nodiscard]] auto read_only_registry( const tool_registry& parent )
		-> result< tool_registry >;

	// On cancellation the partial output is returned with `truncated` set.
	[[nodiscard]] auto run_subagent( const subagent_request& request )
		-> result< subagent_result >;

	// The boundary is enforced by the registry, not by this prompt.
	[[nodiscard]] auto subagent_system_prompt( ) -> std::string_view;

}
