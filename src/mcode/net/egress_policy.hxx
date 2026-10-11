#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::net {

	enum class egress_decision {
		allow,
		deny,
	};

	// An exact host, or a single leading `*.` wildcard. A bare `*` is refused:
	// allowlisting everything is the absence of a policy, and writing it down as
	// one makes it look like a decision.
	struct egress_rule {
		std::string host;
		std::uint16_t port = 443;
		bool allow = true;
	};

	// An allowlist raises the cost of exfiltration; it does not stop it. Anything
	// on the list can carry data out in a URL, a body or a package name, and
	// domain fronting defeats name-based filtering outright. What it removes is the
	// agent's ability to CHOOSE an arbitrary destination, which is the part that
	// makes an injected instruction actionable.
	class egress_policy {
	public:
		// A default-constructed policy denies everything, which is the same as
		// `deny_all`: the safe state must be the one you get by not deciding.
		egress_policy( ) = default;

		[[nodiscard]] static auto deny_all( ) -> egress_policy;

		[[nodiscard]] static auto from_rules( std::vector< egress_rule > rules )
			-> egress_policy;

		[[nodiscard]] auto decide( std::string_view host, std::uint16_t port ) const
			-> egress_decision;

		// A resolved address the sandbox must never dial, independent of the host
		// list: loopback, link-local, multicast, and the cloud metadata endpoints.
		// This is what stops `http://169.254.169.254/` and `http://localhost:8080`
		// from being reachable through an allowlisted name.
		[[nodiscard]] static auto address_is_denied( std::string_view literal ) -> bool;

		[[nodiscard]] auto rules( ) const noexcept -> const std::vector< egress_rule >& {
			return rules_;
		}

	private:
		std::vector< egress_rule > rules_;
	};

	// The port a `CONNECT` tunnel may target. RFC 9110 warns explicitly about
	// tunnels to reserved ports, so anything outside the web set is refused.
	[[nodiscard]] auto connect_port_allowed( std::uint16_t port ) noexcept -> bool;

}
