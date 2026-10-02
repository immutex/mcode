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

	// a disabled server is not started, and one that fails to start is reported on stderr
	struct connect_input {
		// null when a caller has no config layer; tests drive server_config via connect_list
		const std::map< std::string, toml::value, std::less<> >* config_values = nullptr;

		// null when extensions are disabled
		const ext::mcp_server_store* extension_servers = nullptr;

		tool_registry* registry = nullptr;
		agent_loop* loop = nullptr;

		// the caller owns it and must outlive the loop: handlers dispatch into these supervisors
		server_set* owned = nullptr;
	};

	struct connect_report {
		std::vector< std::string > started;

		// the message went to stderr; the session continued
		std::vector< std::string > failed;
	};

	// exposed for tests; connect_servers is the production entry
	auto connect_list( const std::vector< server_config >& servers,
		const connect_input& input ) -> result< connect_report >;

	auto connect_servers( const connect_input& input ) -> result< connect_report >;

	// declare before the loop and destroy after it: the loop's handlers dispatch into these
	class server_set {
	public:
		auto add( std::unique_ptr< supervisor > board ) -> void;

		[[nodiscard]] auto all( ) const noexcept -> const std::vector<
			std::unique_ptr< supervisor > >& {
			return boards_;
		}

		// reverse start order, before the destructor would
		auto shutdown_all( ) -> void;

	private:
		std::vector< std::unique_ptr< supervisor > > boards_;
	};

}
