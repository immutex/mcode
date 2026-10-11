#include "mcode/cli/active_loop.hxx"

#include "mcode/agent/loop.hxx"

namespace mcode::cli {

	namespace {

		// One session per process, so one slot. Not a registry: a second concurrent
		// session would be a second process, and a thread-local would hide a bug.
		mcode::agent_loop* g_active_loop = nullptr;

	}

	auto set_active_loop( mcode::agent_loop* loop ) -> void {
		g_active_loop = loop;
	}

	auto active_loop( ) -> mcode::agent_loop* {
		return g_active_loop;
	}

}
