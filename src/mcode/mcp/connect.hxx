#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/ext/api_mcp.hxx"
#include "mcode/mcp/supervisor.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::mcp {

	class server_set;

	// Connects every declared MCP server to a running loop: parse, merge the
	// extension-declared set, start what is enabled, register the tools and the
	// per-tool handlers.
	//
	// One implementation for the shipped binary and the tests. `cli_commands.cxx`
	// calls `connect_servers`; a test calls the same function against the fixture
	// binary, so the wiring cannot drift from what the tests exercise.
	//
	// Failure rules: a malformed `[mcp]` section is a config error surfaced to the
	// caller; a disabled server is not started; a server that fails to start is
	// reported on stderr and the run continues -- one bad server must not kill the
	// session.
	struct connect_input {
		// The merged config's flattened keys. Null when a caller has no config
		// layer; the tests drive `server_config` directly through `connect_list`.
		const std::map< std::string, toml::value, std::less<> >* config_values = nullptr;

		// The store the extension loader filled. Null when extensions are disabled.
		const ext::mcp_server_store* extension_servers = nullptr;

		tool_registry* registry = nullptr;
		agent_loop* loop = nullptr;

		// Receives the supervisor of every server that started. The caller owns
		// it and must outlive the loop: the supervisors own the child processes
		// and the registered handlers dispatch into them.
		server_set* owned = nullptr;
	};

	struct connect_report {
		// One entry per enabled server that started and registered.
		std::vector< std::string > started;

		// One entry per enabled server that failed to start or register. The
		// message went to stderr; the session continued.
		std::vector< std::string > failed;
	};

	// Starts and registers every server in `servers`. Exposed for tests;
	// `connect_servers` is the production entry.
	auto connect_list( const std::vector< server_config >& servers,
		const connect_input& input ) -> result< connect_report >;

	// The production entry: parses the config section, merges the
	// extension-declared servers, and connects the enabled ones.
	auto connect_servers( const connect_input& input ) -> result< connect_report >;

	// Owns the supervisors of every connected server.
	//
	// Declared BEFORE the loop at the call site and destroyed after it: the
	// supervisors own the child processes, and the handlers the loop dispatches
	// into reference them. Destruction runs each supervisor's graceful shutdown
	// -- close stdin, wait, terminate, kill -- so no child outlives mcode.
	class server_set {
	public:
		auto add( std::unique_ptr< supervisor > board ) -> void;

		[[nodiscard]] auto all( ) const noexcept -> const std::vector<
			std::unique_ptr< supervisor > >& {
			return boards_;
		}

		// Explicit shutdown in reverse start order, before the destructor would.
		auto shutdown_all( ) -> void;

	private:
		std::vector< std::unique_ptr< supervisor > > boards_;
	};

}
