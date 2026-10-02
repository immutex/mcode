#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

#include <algorithm>
#include <filesystem>

#include "lua.h"

namespace mcode::ext {

	namespace {


		// a null installed report means discovery never ran; every query answers the empty set.
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

		// a name, never a path: resolution goes through the discovered set.
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

	auto api_surface::handle_skill_register( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.skill.register expects a definition table" );
			lua_error( state );
		}

		const auto definition = lua_absindex( state, 1 );

		auto entry = mcode::skills::skill_entry{ };
		entry.name = read_field_string( state, definition, "name" );
		entry.description = read_field_string( state, definition, "description" );
		entry.origin = mcode::skills::skill_origin::extension;

		auto length = std::size_t{ 0 };
		lua_getfield( state, definition, "body" );

		if ( lua_type( state, -1 ) == LUA_TSTRING ) {
			const auto* text = lua_tolstring( state, -1, &length );
			entry.file = std::filesystem::path{ std::string_view{ text != nullptr ? text : "",
				length } };
		}

		lua_pop( state, 1 );

		if ( entry.name.empty( ) || entry.description.empty( ) || entry.file.empty( ) ) {
			lua_pushnil( state );
			lua_pushliteral( state,
				"a skill definition needs a name, a description and a body" );

			return 2;
		}

		// refused, not overwritten: discovery precedence is project-first.
		const auto& discovered = skills_of( *self );

		const auto collides = std::find_if( discovered.begin( ), discovered.end( ),
			[ & ]( const mcode::skills::skill_entry& other ) {
				return other.name == entry.name;
			} );

		if ( collides != discovered.end( ) ) {
			lua_pushnil( state );
			lua_pushliteral( state, "a skill with that name already exists" );

			return 2;
		}

		if ( self->skill_sink( ) == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no skill sink was installed with this surface" );

			return 2;
		}

		const auto body = std::string_view{ entry.file.string( ) };

		if ( const auto registered = ( *self->skill_sink( ) )( entry, body ); !registered ) {
			lua_pushnil( state );
			lua_pushlstring( state, registered.error( ).msg.data( ),
				registered.error( ).msg.size( ) );

			return 2;
		} else {
			lua_pushinteger( state, static_cast< lua_Integer >( *registered ) );

			return 1;
		}
	}

}
