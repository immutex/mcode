#pragma once

// The permission gate for the gated entry points, and the plumbing the
// namespace files share.
//
// A permission is checked at call time against the CALLING extension's own
// manifest -- never a session-wide flag, and never another extension's grant.
// A denial is environmental failure, so it returns `nil, err` rather than
// raising: the convention `mcp.register` established, and the one an extension
// can handle.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// Pushes `nil, "<permission> permission denied: <what>"` and returns 2.
	// The single place the denial message is spelled, so every gated entry
	// reads identically to an extension author.
	auto deny_permission( lua_State* state, const char* permission, const char* entry )
		-> int;

	// True when the surface's own manifest carries the permission.
	[[nodiscard]] auto manifest_allows( const api_surface& surface,
		const char* permission ) -> bool;

}
