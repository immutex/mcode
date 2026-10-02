#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace mcode {

	enum class errc {
		ok = 0,
		io,
		json,
		protocol,
		tool_failed,
		cancelled,

		// Distinct from `cancelled`: they carry different exit codes.
		budget_exhausted,

		lua_error,
		config,
		unsupported,
	};

	[[nodiscard]] constexpr auto to_string( const errc code ) noexcept -> std::string_view {
		switch ( code ) {
			case errc::ok: return "ok";
			case errc::io: return "io";
			case errc::json: return "json";
			case errc::protocol: return "protocol";
			case errc::tool_failed: return "tool_failed";
			case errc::cancelled: return "cancelled";
			case errc::budget_exhausted: return "budget_exhausted";
			case errc::lua_error: return "lua_error";
			case errc::config: return "config";
			case errc::unsupported: return "unsupported";
		}

		return "unknown";
	}

	struct error {
		errc code = errc::ok;
		std::string msg;
	};

	[[nodiscard]] inline auto fail( const errc code, std::string msg ) -> error {
		return error{ .code = code, .msg = std::move( msg ) };
	}

	template< class T >
	using result = std::expected< T, error >;

	using status = std::expected< void, error >;

}
