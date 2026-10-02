#pragma once

// a denial returns `nil, err` rather than raising: the extension can handle it.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// the one place the denial message is spelled, so every gated entry reads identically.
	auto deny_permission( lua_State* state, const char* permission, const char* entry )
		-> int;

	[[nodiscard]] auto manifest_allows( const api_surface& surface,
		const char* permission ) -> bool;

}
