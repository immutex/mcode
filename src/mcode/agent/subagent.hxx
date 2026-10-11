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

	inline constexpr std::size_t DEFAULT_SUBAGENT_STEPS = 24;

	// What a caller asks for, with nothing about how it is spawned. An extension
	// supplies this much and no more: the capability restriction and the budget
	// carve are the harness's, so they cannot be an argument.
	struct subagent_spec {
		std::string objective;
		std::string context;
		std::size_t max_steps = DEFAULT_SUBAGENT_STEPS;
	};

	struct subagent_report {
		std::string summary;
		std::uint64_t steps = 0;
		bool truncated = false;

		// What the child spent, so the parent can be charged for it. A child on its
		// own budget is free to the run.
		std::uint64_t tokens_used = 0;
		double usd_used = 0.0;
	};

	// Read-only is a capability set, not a prompt: the child's registry is filtered to
	// the read class, so a write call fails schema validation rather than reaching an
	// approval prompt. The child gets a fresh window, never the parent's context.
	struct subagent_request {
		subagent_spec spec;

		agent_loop::dependencies* parent = nullptr;

		std::vector< std::pair< std::string, agent_loop::tool_handler > > handlers;
	};

	[[nodiscard]] auto subagent_tool_names( ) -> const std::vector< std::string_view >&;

	// Fails rather than skipping a tool it could not copy.
	[[nodiscard]] auto read_only_registry( const tool_registry& parent )
		-> result< tool_registry >;

	// On cancellation the partial output is returned with `truncated` set.
	[[nodiscard]] auto run_subagent( const subagent_request& request )
		-> result< subagent_report >;

	// The boundary is enforced by the registry, not by this prompt.
	[[nodiscard]] auto subagent_system_prompt( ) -> std::string_view;

}
