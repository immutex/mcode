#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "mcode/agent/loop.hxx"
#include "mcode/agent/subagent.hxx"
#include "mcode/core/error.hxx"
#include "mcode/ext/api.hxx"
#include "mcode/ext/api_session.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/config.hxx"
#include "mcode/tools/context.hxx"

namespace mcode::cli {

	// Everything an extension surface needs beyond the tool registry. Held as one
	// object because they are installed as a set, so a capability added here reaches
	// both the interactive and the headless path rather than only one of them.
	struct extension_collaborators {
		std::function< mcode::ext::session_state( ) > session_state;
		std::function< mcode::ext::search_result( const std::string& ) > web_searcher;

		// Resolved at call time through `active_loop`, because the surface is
		// installed before the loop exists.
		std::function< mcode::result< mcode::subagent_report >( const mcode::subagent_spec& ) >
			subagent_runner;

		std::function< mcode::result< std::string >( const std::string&, std::int64_t ) >
			bash_runner;

		std::function< void( std::string_view ) > plan_sink;

		std::function< mcode::result< mcode::ext::fetch_result >( const std::string&,
			std::int64_t ) > fetch_runner;

		// One call, so a collaborator cannot be added to this struct and then
		// forgotten at a call site.
		auto apply( mcode::ext::register_context& context ) const -> void;
	};

	// What the collaborators are built from. Every pointer must outlive them.
	struct collaborator_sources {
		mcode::tools::tool_context* tools = nullptr;
		mcode::net::http_client* transport = nullptr;
		mcode::perm::permission_engine* permissions = nullptr;
		const mcode::config::merged_config* config = nullptr;
		const mcode::event_log* log = nullptr;
		const mcode::session_budget* budget = nullptr;
	};

	[[nodiscard]] auto build_collaborators( const collaborator_sources& sources )
		-> extension_collaborators;

}
