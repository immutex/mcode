#pragma once

// `mcode.context.add_instructions`.
//
// Contributes to the system prompt. Budgeted and counted; requires the
// `context` permission, checked against the calling extension's own manifest.
// Over-budget contributions are truncated, not silently dropped. The full
// per-contributor budget accounting is later-batch work; the plumbing lands
// here.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_context_add_instructions( lua_State* state ) -> int;

}
