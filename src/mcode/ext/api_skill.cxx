#include "mcode/ext/api.hxx"

#include <algorithm>

#include "lua.h"

namespace mcode::ext {

	namespace {

		// Same retrieval as api.cxx's helper: the upvalue is the owner surface.
		// Duplicated because the original lives in that file's anonymous
		// namespace and the closure contract is one lightuserdata at index 1.
		auto surface_from( lua_State* state ) -> api_surface* {
			auto* surface = static_cast< api_surface* >(
				lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( surface == nullptr ) {
				lua_pushliteral( state, "host API is not bound" );
				lua_error( state );
			}

			return surface;
		}

		// The discovered entries. A null pointer means discovery never ran --
		// tests, or a host built without the skills subsystem -- and the honest
		// answer to any skill query is the empty set, not a crash.
		[[nodiscard]] auto skills_of( const api_surface& self )
			-> const std::vector< mcode::skills::skill_entry >& {
			static const std::vector< mcode::skills::skill_entry > none{ };

			return self.installed_skills( ) != nullptr
				? self.installed_skills( )->entries : none;
		}

	} // namespace

	auto api_surface::handle_skill_read( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.skill.read expects a skill name" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		const auto name = std::string_view{ text != nullptr ? text : "", length };

		const auto& entries = skills_of( *self );

		// A name, never a path: resolution goes through the discovered set, so
		// this cannot become an arbitrary-file read.
		const auto found = std::find_if( entries.begin( ), entries.end( ),
			[ & ]( const mcode::skills::skill_entry& entry ) {
				return entry.name == name;
			} );

		if ( found == entries.end( ) ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no skill with that name" );

			return 2;
		}

		auto body = mcode::skills::read_skill_body( entries, name );

		if ( !body ) {
			lua_pushnil( state );
			lua_pushlstring( state, body.error( ).msg.data( ), body.error( ).msg.size( ) );

			return 2;
		}

		lua_pushlstring( state, body->data( ), body->size( ) );

		return 1;
	}

	auto api_surface::handle_skill_list( lua_State* state ) -> int {
		auto* self = surface_from( state );

		const auto& entries = skills_of( *self );

		lua_createtable( state, static_cast< int >( entries.size( ) ), 0 );

		auto index = 1;

		for ( const auto& entry : entries ) {
			lua_createtable( state, 0, 4 );

			lua_pushlstring( state, entry.name.data( ), entry.name.size( ) );
			lua_setfield( state, -2, "name" );

			lua_pushlstring( state, entry.description.data( ), entry.description.size( ) );
			lua_setfield( state, -2, "description" );

			lua_pushboolean( state, entry.disable_model_invocation ? 1 : 0 );
			lua_setfield( state, -2, "hidden" );

			lua_pushlstring( state, mcode::skills::to_string( entry.origin ).data( ),
				mcode::skills::to_string( entry.origin ).size( ) );
			lua_setfield( state, -2, "origin" );

			lua_rawseti( state, -2, index );
			++index;
		}

		return 1;
	}

}
