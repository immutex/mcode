#pragma once

// `mcode.fs.read` and `mcode.fs.write`.
//
// These are NOT a second file-tool implementation. Both entries call the same
// `handle_read` / `handle_write` handlers the model's `read` and `write` tools
// run through, so the workspace boundary, the protected-path deny, and the
// permission engine are one code path. A duplicated resolver that drifted would
// write outside the boundary; routing through the handlers makes that drift
// impossible rather than merely tested.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_fs_read( lua_State* state ) -> int;
	auto handle_fs_write( lua_State* state ) -> int;

}
