#include "mcode/ext/api_entries.hxx"

#include <array>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_bash.hxx"
#include "mcode/ext/api_cfg.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/api_context.hxx"
#include "mcode/ext/api_fetch.hxx"
#include "mcode/ext/api_fs.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/api_memory.hxx"
#include "mcode/ext/api_net.hxx"
#include "mcode/ext/api_plan.hxx"
#include "mcode/ext/api_session.hxx"
#include "mcode/ext/api_task.hxx"
#include "mcode/ext/api_timer.hxx"
#include "mcode/ext/defer.hxx"
#include "mcode/ext/notify.hxx"

namespace mcode::ext {

	namespace {

		auto lua_register_tool( lua_State* state ) -> int {
			return surface_from( state )->handle_register( state );
		}

		auto lua_unregister_tool( lua_State* state ) -> int {
			return surface_from( state )->handle_unregister( state );
		}

		auto lua_log_debug( lua_State* state ) -> int {
			return surface_from( state )->handle_log( state, "debug" );
		}

		auto lua_log_info( lua_State* state ) -> int {
			return surface_from( state )->handle_log( state, "info" );
		}

		auto lua_log_warn( lua_State* state ) -> int {
			return surface_from( state )->handle_log( state, "warn" );
		}

		auto lua_log_error( lua_State* state ) -> int {
			return surface_from( state )->handle_log( state, "error" );
		}

		auto lua_register_provider( lua_State* state ) -> int {
			return surface_from( state )->handle_register_provider( state );
		}

		auto lua_on( lua_State* state ) -> int {
			return surface_from( state )->handle_on( state );
		}

		auto lua_off( lua_State* state ) -> int {
			return surface_from( state )->handle_off( state );
		}

		auto lua_emit( lua_State* state ) -> int {
			return surface_from( state )->handle_emit( state );
		}

		auto lua_skill_read( lua_State* state ) -> int {
			return surface_from( state )->handle_skill_read( state );
		}

		auto lua_memory_search( lua_State* state ) -> int {
			return handle_memory_search( state );
		}

		auto lua_skill_list( lua_State* state ) -> int {
			return surface_from( state )->handle_skill_list( state );
		}

		auto lua_skill_register( lua_State* state ) -> int {
			return surface_from( state )->handle_skill_register( state );
		}

		auto lua_mcp_register( lua_State* state ) -> int {
			return surface_from( state )->handle_mcp_register( state );
		}

		auto lua_fs_read( lua_State* state ) -> int {
			return handle_fs_read( state );
		}

		auto lua_fs_write( lua_State* state ) -> int {
			return handle_fs_write( state );
		}

		auto lua_cfg_get( lua_State* state ) -> int {
			return handle_cfg_get( state );
		}

		auto lua_session_snapshot( lua_State* state ) -> int {
			return handle_session_snapshot( state );
		}

		auto lua_session_fork( lua_State* state ) -> int {
			return handle_session_fork( state );
		}

		auto lua_defer( lua_State* state ) -> int {
			return handle_defer( state );
		}

		auto lua_notify( lua_State* state ) -> int {
			return handle_notify( state );
		}

		auto lua_net_get( lua_State* state ) -> int {
			return handle_net_get( state );
		}

		auto lua_net_search( lua_State* state ) -> int {
			return handle_net_search( state );
		}

		auto lua_context_add_instructions( lua_State* state ) -> int {
			return handle_context_add_instructions( state );
		}

		auto lua_task( lua_State* state ) -> int {
			return handle_task( state );
		}

		auto lua_bash( lua_State* state ) -> int {
			return handle_bash( state );
		}

		auto lua_plan_set( lua_State* state ) -> int {
			return handle_plan_set( state );
		}

		auto lua_fetch( lua_State* state ) -> int {
			return handle_fetch( state );
		}

		auto lua_cmd_register( lua_State* state ) -> int {
			return handle_cmd_register( state );
		}

		auto lua_timer_at( lua_State* state ) -> int {
			return handle_timer_at( state );
		}

		auto lua_timer_every( lua_State* state ) -> int {
			return handle_timer_every( state );
		}

	}

	// Ordered as the type stub declares the surface, so the two read side by side.
	inline constexpr auto ENTRIES = std::array< api_entry, 32 >{
		api_entry{ "tool.register", lua_register_tool },
		api_entry{ "tool.unregister", lua_unregister_tool },
		api_entry{ "model.register", lua_register_provider },
		api_entry{ "on", lua_on },
		api_entry{ "off", lua_off },
		api_entry{ "emit", lua_emit },
		api_entry{ "log.debug", lua_log_debug },
		api_entry{ "log.info", lua_log_info },
		api_entry{ "log.warn", lua_log_warn },
		api_entry{ "log.error", lua_log_error },
		api_entry{ "skill.read", lua_skill_read },
		api_entry{ "memory.search", lua_memory_search },
		api_entry{ "skill.list", lua_skill_list },
		api_entry{ "skill.register", lua_skill_register },
		api_entry{ "mcp.register", lua_mcp_register },
		api_entry{ "fs.read", lua_fs_read },
		api_entry{ "fs.write", lua_fs_write },
		api_entry{ "cfg.get", lua_cfg_get },
		api_entry{ "cmd.register", lua_cmd_register },
		api_entry{ "timer.at", lua_timer_at },
		api_entry{ "timer.every", lua_timer_every },
		api_entry{ "session.snapshot", lua_session_snapshot },
		api_entry{ "session.fork", lua_session_fork },
		api_entry{ "defer", lua_defer },
		api_entry{ "notify", lua_notify },
		api_entry{ "net.get", lua_net_get },
		api_entry{ "net.search", lua_net_search },
		api_entry{ "context.add_instructions", lua_context_add_instructions },
		api_entry{ "task", lua_task },
		api_entry{ "bash", lua_bash },
		api_entry{ "plan.set", lua_plan_set },
		api_entry{ "fetch", lua_fetch },
	};

	auto api_entries( ) -> std::span< const api_entry > {
		return ENTRIES;
	}

}
