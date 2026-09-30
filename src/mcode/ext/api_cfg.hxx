#pragma once

// `mcode.cfg.get`.
//
// Read-only and scoped: an extension reads its own `[extensions.<name>]`
// section, never another extension's and never the harness's own keys. The
// config object travels with the surface; the entry reports an uninstalled
// config rather than guessing.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_cfg_get( lua_State* state ) -> int;

}
