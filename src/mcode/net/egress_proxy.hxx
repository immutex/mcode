#pragma once

#include <cstdint>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/net/egress_policy.hxx"

namespace mcode::net {

	// Two listeners, both bound to loopback on ephemeral ports: HTTP `CONNECT` for HTTPS and
	// absolute-form HTTP, and SOCKS5 for everything else TCP.
	class egress_proxy {
	public:
		~egress_proxy( );

		egress_proxy( egress_proxy&& other ) noexcept;
		auto operator=( egress_proxy&& other ) noexcept -> egress_proxy&;

		egress_proxy( const egress_proxy& ) = delete;
		auto operator=( const egress_proxy& ) -> egress_proxy& = delete;

		// Starts on ephemeral loopback ports. Fails closed: no listener, no proxy.
		[[nodiscard]] static auto start( egress_policy policy ) -> result< egress_proxy >;

		auto serve( ) -> void;

		auto stop( ) -> void;

		[[nodiscard]] auto http_port( ) const noexcept -> std::uint16_t;
		[[nodiscard]] auto socks_port( ) const noexcept -> std::uint16_t;

		// The OS rule is the boundary; `curl --noproxy '*'` walks straight past these.
		[[nodiscard]] auto environment( ) const -> std::string;

		[[nodiscard]] auto denied_count( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto allowed_count( ) const noexcept -> std::uint64_t;

	private:
		egress_proxy( ) = default;

		struct implementation;
		implementation* self_ = nullptr;
	};

}
