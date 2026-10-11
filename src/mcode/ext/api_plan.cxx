#include "mcode/ext/api_plan.hxx"

#include <cstddef>
#include <string_view>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// The plan is recited into the prompt on an interval, so it is a budget line,
		// not free text: a plan longer than this crowds out the history it summarizes.
		constexpr std::size_t MAX_PLAN_CHARS = 8 * 1024;

	}

	// `mcode.plan.set(text)` -> true, err
	//
	// The plan lives in the loop, not in the extension that maintains it, so
	// disabling `todo` loses the tool and never the plan.
	auto handle_plan_set( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.plan.set expects the plan as a string" );

			return 2;
		}

		auto* sink = self->plan_sink( );

		if ( sink == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no plan sink was installed with this surface" );

			return 2;
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		auto plan = std::string_view{ text != nullptr ? text : "", length };

		if ( plan.size( ) > MAX_PLAN_CHARS ) {
			plan = plan.substr( 0, MAX_PLAN_CHARS );
		}

		( *sink )( plan );

		lua_pushboolean( state, 1 );

		return 1;
	}

}
