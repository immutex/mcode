#pragma once

#include <functional>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/ext/api.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli {

	// A search provider configured under `[web]`, or null when none is. The HTTP
	// call, the host allowlist and the size cap are the harness's; only the endpoint
	// shape and the response field names come from configuration. Absent
	// configuration yields null rather than a tool that exists only to fail.
	[[nodiscard]] auto make_web_searcher( const mcode::config::merged_config& config,
		mcode::net::http_client& client )
		-> std::function< mcode::ext::search_result( const std::string& ) >;

}
