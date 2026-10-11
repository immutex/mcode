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

	// A read-only subagent. The restriction is a capability set, not a prompt: the
	// child's registry is built from the parent's and filtered to the read class,
	// so a write call fails schema validation rather than reaching an approval
	// prompt. Cline's Plan mode is the cautionary case -- it still runs a shell,
	// and `python -c` writes files.
	//
	// The child gets a FRESH window, not a fork of the parent's history. That is
	// the point: it explores tens of thousands of tokens and returns a summary, so
	// the trajectory never enters the main context. It also makes the child dumber
	// about the task by design, which is why the spawn payload has to carry the
	// constraints explicitly.
	struct subagent_request {
		std::string objective;
		std::string context;

		// The parent's deps, borrowed. The child is constructed from them so the
		// model client, log, bus and budget accounting stay one system.
		agent_loop::dependencies* parent = nullptr;

		// Handlers for the read-class tools the parent installed. The child needs
		// its own copies because the loop owns its handler table.
		std::vector< std::pair< std::string, agent_loop::tool_handler > > handlers;

		std::size_t max_steps = 24;
	};

	struct subagent_result {
		std::string summary;
		std::uint64_t steps = 0;
		bool truncated = false;
	};

	// The child's tools: read class only. One definition, so the filter and the
	// documentation cannot drift.
	[[nodiscard]] auto subagent_tool_names( ) -> const std::vector< std::string_view >&;

	// Builds the read-only registry from the parent's, dropping everything the
	// child must not reach.
	[[nodiscard]] auto read_only_registry( const tool_registry& parent ) -> tool_registry;

	// Runs the child to completion and harvests its final answer. On cancellation
	// the partial output is returned with `truncated` set rather than discarded: a
	// 150K-token exploration is worth keeping even when the budget ran out.
	[[nodiscard]] auto run_subagent( const subagent_request& request )
		-> result< subagent_result >;

	// The child's system prompt. Read-only is stated because the model should not
	// waste a step discovering the boundary, but the boundary does not depend on it.
	[[nodiscard]] auto subagent_system_prompt( ) -> std::string_view;

}
