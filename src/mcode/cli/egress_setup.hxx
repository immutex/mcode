#pragma once

#include <memory>
#include <string>

#include "mcode/net/egress_proxy.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli {

	// Starts the loopback proxy when the configuration asks for one.
	//
	// The default is no proxy, which means the child gets no network at all: an
	// allowlist is a grant, and a grant has to be asked for. Returning null is the
	// common case and is not an error.
	[[nodiscard]] auto start_egress_if_configured( const config::merged_config& values )
		-> std::unique_ptr< net::egress_proxy >;

}
