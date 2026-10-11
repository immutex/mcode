#pragma once

#include <memory>
#include <string>

#include "mcode/net/egress_proxy.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli {

	// The default is no proxy, so the child gets no network at all: an allowlist is a
	// grant, and a grant must be asked for.
	[[nodiscard]] auto start_egress_if_configured( const config::merged_config& values )
		-> std::unique_ptr< net::egress_proxy >;

}
