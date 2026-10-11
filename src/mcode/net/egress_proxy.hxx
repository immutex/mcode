#pragma once

#include <cstdint>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/net/egress_policy.hxx"

namespace mcode::net {

	// A loopback proxy the sandboxed child is pointed at. It is not a data-loss
	// prevention control: it does not inspect the traffic, and anything on the
	// allowlist can carry data out. What it removes is the agent's ability to
	// CHOOSE an arbitrary destination.
	//
	// Two listeners: HTTP `CONNECT` for HTTPS and absolute-form HTTP, and SOCKS5
	// for everything else TCP. Both are bound to loopback on ephemeral ports, so
	// nothing outside this process can reach them.
	class egress_proxy {
	public:
		~egress_proxy( );

		egress_proxy( egress_proxy&& other ) noexcept;
		auto operator=( egress_proxy&& other ) noexcept -> egress_proxy&;

		egress_proxy( const egress_proxy& ) = delete;
		auto operator=( const egress_proxy& ) -> egress_proxy& = delete;

		// Starts on ephemeral loopback ports. Fails closed: no listener, no proxy.
		[[nodiscard]] static auto start( egress_policy policy ) -> result< egress_proxy >;

		// Serves until `stop` is called. Runs on its own thread.
		auto serve( ) -> void;

		auto stop( ) -> void;

		[[nodiscard]] auto http_port( ) const noexcept -> std::uint16_t;
		[[nodiscard]] auto socks_port( ) const noexcept -> std::uint16_t;

		// The environment a sandboxed child needs to find the proxy. The OS rule is
		// the boundary; these variables are only how a compliant tool discovers it,
		// and `curl --noproxy '*'` walks straight past them.
		[[nodiscard]] auto environment( ) const -> std::string;

		[[nodiscard]] auto denied_count( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto allowed_count( ) const noexcept -> std::uint64_t;

	private:
		egress_proxy( ) = default;

		struct implementation;
		implementation* self_ = nullptr;
	};

}
