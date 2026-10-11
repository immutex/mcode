#include "mcode/ext/api_task.hxx"

#include <cstddef>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		constexpr std::size_t MAX_OBJECTIVE_CHARS = 4096;
		constexpr std::size_t MAX_CONTEXT_CHARS = 16 * 1024;
		constexpr std::size_t MAX_STEPS_LIMIT = 64;

	}

	// `mcode.task(objective, opts)` -> report, err
	//
	// The child is read-only by construction: the core filters its registry to the
	// read class, so no argument can widen what it may do.
	auto handle_task( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.task expects the objective as a string" );
			lua_error( state );
		}

		if ( !argument_absent( state, 2 ) && lua_type( state, 2 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.task: opts must be a table or nil" );
			lua_error( state );
		}

		auto* runner = self->subagent_runner( );

		if ( runner == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no subagent runner was installed with this surface" );

			return 2;
		}

		auto length = std::size_t{ 0 };
		const auto* objective_text = lua_tolstring( state, 1, &length );

		auto spec = mcode::subagent_spec{ };
		spec.objective.assign( objective_text != nullptr ? objective_text : "", length );

		if ( spec.objective.size( ) > MAX_OBJECTIVE_CHARS ) {
			spec.objective.resize( MAX_OBJECTIVE_CHARS );
		}

		if ( lua_type( state, 2 ) == LUA_TTABLE ) {
			const auto options = lua_absindex( state, 2 );

			spec.context = read_field_string( state, options, "context" );

			if ( spec.context.size( ) > MAX_CONTEXT_CHARS ) {
				spec.context.resize( MAX_CONTEXT_CHARS );
			}

			lua_getfield( state, options, "max_steps" );

			if ( lua_type( state, -1 ) == LUA_TNUMBER ) {
				const auto requested = lua_tonumber( state, -1 );

				if ( requested > 0.0 ) {
					const auto capped = requested < static_cast< double >( MAX_STEPS_LIMIT )
						? requested : static_cast< double >( MAX_STEPS_LIMIT );
					spec.max_steps = static_cast< std::size_t >( capped );
				}
			}

			lua_pop( state, 1 );
		}

		const auto report = ( *runner )( spec );

		if ( !report ) {
			lua_pushnil( state );
			lua_pushlstring( state, report.error( ).msg.data( ), report.error( ).msg.size( ) );

			return 2;
		}

		lua_createtable( state, 0, 3 );

		lua_pushlstring( state, report->summary.data( ), report->summary.size( ) );
		lua_setfield( state, -2, "summary" );

		lua_pushnumber( state, static_cast< double >( report->steps ) );
		lua_setfield( state, -2, "steps" );

		lua_pushboolean( state, report->truncated ? 1 : 0 );
		lua_setfield( state, -2, "truncated" );

		return 1;
	}

}
