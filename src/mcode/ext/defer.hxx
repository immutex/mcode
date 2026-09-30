#pragma once

// `mcode.defer`.
//
// Defers `fn` to the next loop iteration. Use this to leave a callback
// context before touching host state -- the `vim.schedule` lesson. The
// deferred queue is per-surface and drained on the loop thread between
// iterations; a deferred closure dies with its VM, so it can never run after
// the extension is gone.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_defer( lua_State* state ) -> int;

}
