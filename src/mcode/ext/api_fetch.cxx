#include "mcode/ext/api_fetch.hxx"

#include <cstddef>
#include <cstdint>
#include <string>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		constexpr std::size_t MAX_URL_CHARS = 2048;

		constexpr std::int64_t MIN_TIMEOUT_MS = 1000;
		constexpr std::int64_t MAX_TIMEOUT_MS = 120'000;
		constexpr std::int64_t DEFAULT_TIMEOUT_MS = 30'000;

	}

	// `mcode.fetch(url, opts)` -> { status, content_type, body, truncated }, err
	//
	// For a URL the MODEL supplied, so the extension's own `net:<host>` allowlist
	// cannot apply; the per-domain decision and the private-address filter replace it.
	auto handle_fetch( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( !manifest_allows( *self, "net" ) ) {
			return deny_permission( state, "net", "mcode.fetch" );
		}

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.fetch expects the URL as a string" );

			return 2;
		}

		if ( !argument_absent( state, 2 ) && lua_type( state, 2 ) != LUA_TTABLE ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.fetch: opts must be a table or nil" );

			return 2;
		}

		auto* runner = self->fetch_runner( );

		if ( runner == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no fetch runner was installed with this surface" );

			return 2;
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		if ( text == nullptr || length == 0 ) {
			lua_pushnil( state );
			lua_pushliteral( state, "url is required" );

			return 2;
		}

		if ( length > MAX_URL_CHARS ) {
			lua_pushnil( state );
			lua_pushliteral( state, "url is too long" );

			return 2;
		}

		auto url = std::string{ text, length };
		auto timeout_ms = DEFAULT_TIMEOUT_MS;

		if ( lua_type( state, 2 ) == LUA_TTABLE ) {
			const auto options = lua_absindex( state, 2 );

			lua_getfield( state, options, "timeout_ms" );

			if ( lua_type( state, -1 ) == LUA_TNUMBER ) {
				const auto requested = static_cast< std::int64_t >( lua_tonumber( state, -1 ) );

				if ( requested >= MIN_TIMEOUT_MS ) {
					timeout_ms = requested < MAX_TIMEOUT_MS ? requested : MAX_TIMEOUT_MS;
				}
			}

			lua_pop( state, 1 );
		}

		const auto fetched = ( *runner )( url, timeout_ms );

		if ( !fetched ) {
			lua_pushnil( state );
			lua_pushlstring( state, fetched.error( ).msg.data( ),
				fetched.error( ).msg.size( ) );

			return 2;
		}

		lua_createtable( state, 0, 4 );

		lua_pushnumber( state, static_cast< double >( fetched->status ) );
		lua_setfield( state, -2, "status" );

		lua_pushlstring( state, fetched->content_type.data( ), fetched->content_type.size( ) );
		lua_setfield( state, -2, "content_type" );

		lua_pushlstring( state, fetched->body.data( ), fetched->body.size( ) );
		lua_setfield( state, -2, "body" );

		lua_pushboolean( state, fetched->truncated ? 1 : 0 );
		lua_setfield( state, -2, "truncated" );

		return 1;
	}

}
