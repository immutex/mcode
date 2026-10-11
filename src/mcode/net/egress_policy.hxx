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

	// An exact host, or a single leading `*.` wildcard; a bare `*` is refused, because
	// allowlisting everything written down as a rule looks like a decision.
	struct egress_rule {
		std::string host;
		std::uint16_t port = 443;
		bool allow = true;
	};

	class egress_policy {
	public:
		// A default-constructed policy denies everything: the safe state is the one you
		// get by not deciding.
		egress_policy( ) = default;

		[[nodiscard]] static auto deny_all( ) -> egress_policy;

		[[nodiscard]] static auto from_rules( std::vector< egress_rule > rules )
			-> egress_policy;

		[[nodiscard]] auto decide( std::string_view host, std::uint16_t port ) const
			-> egress_decision;

		// Independent of the host list: loopback, link-local, multicast, cloud metadata.
		[[nodiscard]] static auto address_is_denied( std::string_view literal ) -> bool;

		[[nodiscard]] auto rules( ) const noexcept -> const std::vector< egress_rule >& {
			return rules_;
		}

	private:
		std::vector< egress_rule > rules_;
	};

	// RFC 9110 warns about tunnels to reserved ports, so anything outside the web set is
	// refused.
	[[nodiscard]] auto connect_port_allowed( std::uint16_t port ) noexcept -> bool;

}
