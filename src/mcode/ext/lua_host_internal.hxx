#pragma once

// lua.h is deliberately not included: `lua_State` is named here only as an incomplete type.

#include <cstdint>
#include <string>
#include <string_view>

#include "mcode/ext/lua_host.hxx"

struct lua_State;

namespace mcode::ext::detail {

	// the interrupt fires at every loop back edge and call, so the check is sampled.
	inline constexpr auto INTERRUPT_GRANULARITY = std::uint64_t{ 10'000 };

	// the addresses are the registry keys, so each needs static storage.
	inline char g_watchdog_key = 0;
	inline char g_module_loader_key = 0;

	[[nodiscard]] auto pop_error( lua_State* state ) -> std::string;

	[[nodiscard]] auto registry_pointer( lua_State* state, void* key ) -> void*;

	[[nodiscard]] auto watchdog_from( lua_State* state ) -> ::mcode::detail::watchdog_state*;

	[[nodiscard]] auto loader_from( lua_State* state ) -> ::mcode::module_loader_function*;

	auto interrupt( lua_State* state, int gc ) -> void;
	auto host_function_dispatch( lua_State* state ) -> int;

	[[nodiscard]] auto module_require( lua_State* state ) -> int;

	// leaves the callable closure on top of the stack; false fills `message`.
	[[nodiscard]] auto load_chunk( lua_State* thread, std::string_view source,
		std::string_view chunk_name, std::string& message ) -> bool;

}
