#pragma once

// `mcode.notify`.
//
// A user-visible, attributed message. Always available: notification is
// presentation, not capability. Attribution is automatic and non-optional --
// the extension name comes from the surface, so a handler cannot forge
// another extension's attribution. The message lands where the host's
// notifier sends it; with no notifier installed the call is a counted no-op
// rather than a silent drop.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_notify( lua_State* state ) -> int;

}
