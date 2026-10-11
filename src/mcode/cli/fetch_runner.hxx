#pragma once

#include <functional>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/ext/api.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli {

	// A fetcher for URLs the MODEL chose, as opposed to ones an extension chose.
	// `net.get` is bounded by the extension's static `net:<host>` declarations, which
	// cannot cover a host the model invents; this is bounded by the permission engine
	// per domain plus the refusal of private addresses. A null engine fails closed.
	[[nodiscard]] auto make_fetch_runner( const mcode::config::merged_config& config,
		mcode::net::http_client& client, mcode::perm::permission_engine* permissions )
		-> std::function< mcode::result< mcode::ext::fetch_result >( const std::string&,
			std::int64_t ) >;

}
