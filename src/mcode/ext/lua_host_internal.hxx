#pragma once

// Shared by lua_host.cxx and lua_host_execute.cxx. The two were one file past the
// 600-line limit; these are the helpers they both need.
//
// The Lua API is deliberately NOT included here: `lua.h` is a private dependency of
// the .cxx files, and a header that pulled it in would leak it to every consumer.
// The declarations below name `lua_State` only as an incomplete type.

#include <cstdint>
#include <string>
#include <string_view>

#include "mcode/ext/lua_host.hxx"

struct lua_State;

namespace mcode::ext::detail {

	// The interrupt runs on every loop back edge and every call, so the check is
	// sampled rather than run every time. Ten thousand is the granularity the spike
	// measured as free.
	inline constexpr auto INTERRUPT_GRANULARITY = std::uint64_t{ 10'000 };

	// Registry keys. Their ADDRESSES are the keys, so each must be a distinct
	// object with static storage -- which is why they are inline variables rather
	// than constants.
	inline char g_watchdog_key = 0;
	inline char g_module_loader_key = 0;

	// Pops the error message from the top of the stack.
	[[nodiscard]] auto pop_error( lua_State* state ) -> std::string;

	[[nodiscard]] auto registry_pointer( lua_State* state, void* key ) -> void*;

	[[nodiscard]] auto watchdog_from( lua_State* state ) -> ::mcode::detail::watchdog_state*;

	[[nodiscard]] auto loader_from( lua_State* state ) -> ::mcode::module_loader_function*;

	// The interrupt callback installed on the VM, and the trampoline every host
	// function is registered through. Lua-facing, so they live with the other
	// entry points rather than with the host's own bookkeeping.
	auto interrupt( lua_State* state, int gc ) -> void;
	auto host_function_dispatch( lua_State* state ) -> int;

	// `require`.
	[[nodiscard]] auto module_require( lua_State* state ) -> int;

	// Compiles and loads a chunk onto the thread, leaving the callable closure on
	// top. Returns false and fills `message` on failure.
	//
	// One implementation because `run`, `eval_to_string` and the module loader all
	// need it, and the two hand-written copies that existed had already drifted.
	[[nodiscard]] auto load_chunk( lua_State* thread, std::string_view source,
		std::string_view chunk_name, std::string& message ) -> bool;

}
