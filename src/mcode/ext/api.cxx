#include "mcode/ext/api.hxx"

#include <cstdio>
#include <string>
#include <utility>

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstdint>
#include <cstddef>

#include "mcode/ext/lua_json.hxx"

namespace mcode::ext {

	const model::provider_registry api_surface::empty_providers_{ };

	namespace {

		// The two frozen identity fields: `mcode.ext.name` and `mcode.api_version`.
		// Nothing else is written into the surface, because the freeze is closed and
		// an unlisted field is exactly the drift the table exists to prevent.
		constexpr auto EXTENSION_NAME_PATH = "ext.name";
		constexpr auto API_VERSION_PATH = "api_version";

		// Bounded so a runaway extension cannot fill the log. The count is what
		// matters; the text is best-effort.
		constexpr auto MAX_LOGGED_ARGUMENTS = 8;
		constexpr auto MAX_LOGGED_LENGTH = 2048;

		auto to_text( lua_State* state, const int index ) -> std::string {
			if ( lua_type( state, index ) == LUA_TSTRING ||
				lua_type( state, index ) == LUA_TNUMBER ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, index, &length );

				if ( text != nullptr ) {
					auto out = std::string{ text, length };

					if ( out.size( ) > MAX_LOGGED_LENGTH ) {
						out.resize( MAX_LOGGED_LENGTH );
						out += "...";
					}

					return out;
				}
			}

			return std::string{ lua_typename( state, lua_type( state, index ) ) };
		}

		// Returns the api_surface bound as the closure's upvalue. Every raw entry
		// point needs it, and a missing upvalue is a host bug rather than an
		// extension error -- reported as an error rather than dereferenced.
		auto surface_from( lua_State* state ) -> api_surface* {
			auto* surface = static_cast< api_surface* >(
				lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( surface == nullptr ) {
				lua_pushliteral( state, "host API is not bound" );
				lua_error( state );
			}

			return surface;
		}

		auto read_field_string( lua_State* state, const int table, const char* key )
			-> std::string {
			lua_getfield( state, table, key );

			auto value = std::string{ };

			if ( lua_type( state, -1 ) == LUA_TSTRING ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, -1, &length );

				if ( text != nullptr ) {
					value.assign( text, length );
				}
			}

			lua_pop( state, 1 );

			return value;
		}

	}

	auto api_surface::handle_register( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.tool.register expects a definition table" );
			lua_error( state );
		}

		const auto definition = lua_absindex( state, 1 );

		auto tool = registered_tool{ };
		tool.name = read_field_string( state, definition, "name" );
		tool.description = read_field_string( state, definition, "description" );
		tool.owner = self->manifest_.name;

		if ( tool.name.empty( ) ) {
			lua_pushliteral( state, "mcode.tool.register: 'name' is required" );
			lua_error( state );
		}

		if ( tool.description.empty( ) ) {
			lua_pushliteral( state, "mcode.tool.register: 'description' is required" );
			lua_error( state );
		}

		// The schema is validated by being encodable, and rendered once here.
		lua_getfield( state, definition, "schema" );

		if ( lua_type( state, -1 ) != LUA_TTABLE ) {
			lua_pop( state, 1 );

			lua_pushliteral( state, "mcode.tool.register: 'schema' must be a table" );
			lua_error( state );
		}

		// The schema is data. Rendering it here rather than per turn keeps the
		// prompt cache stable, and it is the only place the host walks
		// it.
		auto schema = std::string{ };
		{
			// A dedicated encoder call: the schema is plain JSON data, and the
			// helper below is the same one the argument path uses.
			lua_pushvalue( state, -1 );
			auto rendered = json_from_lua( state, -1 );
			lua_pop( state, 1 );

			if ( !rendered ) {
				lua_pop( state, 1 );

				const auto message = std::string{ "mcode.tool.register: schema is not "
					"encodable: " } + rendered.error( ).msg;

				lua_pushlstring( state, message.data( ), message.size( ) );
				lua_error( state );
			}

			schema = *rendered;
		}

		lua_pop( state, 1 );

		// The `run` closure is kept by registry reference. It cannot be copied into
		// C++: a Luau function is a value in this VM, and the reference is what
		// keeps it alive while the registry holds the tool.
		lua_getfield( state, definition, "run" );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_pop( state, 1 );

			lua_pushliteral( state, "mcode.tool.register: 'run' must be a function" );
			lua_error( state );
		}

		tool.function_reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		auto definition_value = tool_def{ };
		definition_value.name = tool.name;
		definition_value.description = tool.description;
		definition_value.klass = tool_class::read;
		definition_value.source = tool_source::user_extension;
		definition_value.owner = tool.owner;
		definition_value.deferrable = true;
		definition_value.schema_json = std::move( schema );

		// A name collision is a contract violation: the caller gets an
		// error, not a silently replaced tool. The reference is released first so
		// a rejected registration does not leak it.
		if ( auto added = self->registry_->add( std::move( definition_value ) ); !added ) {
			lua_unref( state, tool.function_reference );

			const auto message = std::string{ "mcode.tool.register: " } + added.error( ).msg;

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		self->by_name_.insert_or_assign( tool.name, self->tools_.size( ) );
		self->tools_.push_back( std::move( tool ) );

		lua_pushnumber( state, static_cast< double >( self->tools_.size( ) ) );

		return 1;
	}

	auto api_surface::handle_unregister( lua_State* state ) -> int {
		auto* self = surface_from( state );

		// Truncating the double would silently address the wrong tool for a
		// fractional id, and the cast is undefined outside the representable range.
		const auto number = luaL_checknumber( state, 1 );
		const auto identifier = static_cast< std::int64_t >( number );

		if ( static_cast< double >( identifier ) != number || identifier <= 0 ||
			static_cast< std::size_t >( identifier ) > self->tools_.size( ) ) {
			lua_pushliteral( state, "mcode.tool.unregister: unknown id" );
			lua_error( state );
		}

		const auto index = static_cast< std::size_t >( identifier ) - 1;
		const auto& tool = self->tools_[ index ];

		// Removing the registry entry by owner would drop every tool the
		// extension registered, so the one tool is removed by name and the
		// closure reference is released with it.
		self->registry_->remove( tool.name );

		lua_unref( state, tool.function_reference );

		self->tools_.erase( self->tools_.begin( ) + static_cast< std::ptrdiff_t >( index ) );

		self->by_name_.clear( );

		for ( auto position = std::size_t{ 0 }; position < self->tools_.size( ); ++position ) {
			self->by_name_.insert_or_assign( self->tools_[ position ].name, position );
		}

		return 0;
	}

	auto api_surface::handle_log( lua_State* state, const char* level ) -> int {
		auto* self = surface_from( state );

		auto message = std::string{ };

		message += "[" + self->manifest_.name + "] ";

		message += level;
		message += ": ";

		const auto count = lua_gettop( state );

		for ( auto index = 1; index <= count && index <= MAX_LOGGED_ARGUMENTS; ++index ) {
			if ( index > 1 ) {
				message += ' ';
			}

			message += to_text( state, index );
		}

		// stderr, not the session log: an extension's log line is diagnostic
		// output, and routing it into the event log would let extension code
		// write arbitrary entries into the transcript.
		std::fprintf( stderr, "%s\n", message.c_str( ) );

		return 0;
	}

	auto api_surface::handle_register_provider( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.model.register expects a descriptor table" );
			lua_error( state );
		}

		// The descriptor is data, so it crosses the boundary as JSON and
		// is parsed by the same validator the C++ tests use. A second parser here
		// would be a second set of rules.
		auto rendered = json_from_lua( state, 1 );

		if ( !rendered ) {
			const auto message = std::string{ "mcode.model.register: the descriptor is not "
				"encodable: " } + rendered.error( ).msg;

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		auto descriptor = model::descriptor_from_json( *rendered );

		if ( !descriptor ) {
			const auto message = std::string{ "mcode.model.register: " } +
				descriptor.error( ).msg;

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		// `net` is required to declare a provider: a descriptor names an
		// endpoint, and declaring one without the permission to reach it would
		// defer the denial to the first request.
		if ( !self->manifest_.has_permission( "net" ) ) {
			const auto message = std::string{ "mcode.model.register: extension '" } +
				self->manifest_.name + "' declares no 'net' permission";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		if ( auto added = self->providers_->add( std::move( *descriptor ) ); !added ) {
			const auto message = std::string{ "mcode.model.register: " } + added.error( ).msg;

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		lua_pushboolean( state, 1 );

		return 1;
	}

	auto api_surface::handle_on( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.on: the event name must be a string" );
			lua_error( state );
		}

		auto name_length = std::size_t{ 0 };
		const auto* name_text = lua_tolstring( state, 1, &name_length );

		if ( name_text == nullptr || name_length == 0 ) {
			lua_pushliteral( state, "mcode.on: the event name must not be empty" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TFUNCTION ) {
			lua_pushliteral( state, "mcode.on: the handler must be a function" );
			lua_error( state );
		}

		const auto name = std::string_view{ name_text, name_length };

		// `tool.precal` is a typo, not a custom event, and a hook that can never fire
		// is worse than a load error -- the author would believe their guard was
		// active. A name whose first segment belongs to the session vocabulary must
		// be a session event.
		if ( ext::has_reserved_prefix( name ) ) {
			const auto message = std::string{ "mcode.on: '" } + std::string{ name } +
				"' is not a session event, but '" +
				std::string{ name.substr( 0, name.find( '.' ) ) } +
				"' is a session event namespace -- did you mistype one?";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		// A custom name must be namespaced, so it cannot collide with a future
		// session event and cannot be mistaken for one.
		if ( !ext::session_event_kind( name ) && name.find( '.' ) == std::string_view::npos ) {
			const auto message = std::string{ "mcode.on: '" } + std::string{ name } +
				"' is not a session event and is not namespaced like a custom event "
				"(expected something like 'myext.ready')";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		// The handler is kept by reference, for the same reason a tool's `run` is:
		// a Luau function is a value in this VM and cannot be copied into C++.
		lua_pushvalue( state, 2 );
		const auto reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		auto identifier = self->hooks_->subscribe( *self->host_, name, reference,
			self->manifest_.name );

		if ( !identifier ) {
			lua_unref( state, reference );

			lua_pushlstring( state, identifier.error( ).msg.data( ),
				identifier.error( ).msg.size( ) );
			lua_error( state );
		}

		lua_pushnumber( state, static_cast< double >( *identifier ) );

		return 1;
	}

	auto api_surface::handle_off( lua_State* state ) -> int {
		auto* self = surface_from( state );

		const auto number = luaL_checknumber( state, 1 );

		// A fractional or non-positive id addresses nothing. Truncating it would
		// unsubscribe whatever id the cast happened to land on.
		if ( number < 1.0 || number != std::floor( number ) ) {
			return 0;
		}

		const auto identifier = static_cast< std::uint64_t >( number );

		// Unknown ids are a no-op rather than an error: unsubscribing twice is a
		// legitimate pattern, and a raise here would make teardown code fragile.
		self->hooks_->unsubscribe( identifier );

		return 0;
	}

	auto api_surface::handle_emit( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.emit: the event name must be a string" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		const auto name = std::string_view{ text != nullptr ? text : "", length };

		// An extension may only emit under its OWN namespace. Subscribing to another
		// extension's events is legitimate; emitting them is spoofing, and the two
		// are deliberately asymmetric.
		{
			const auto separator = name.find( '.' );
			const auto prefix = separator == std::string_view::npos
				? name : name.substr( 0, separator );

			if ( prefix != self->manifest_.name ) {
				const auto message = std::string{ "mcode.emit: '" } + std::string{ name } +
					"' is outside this extension's namespace ('" + self->manifest_.name +
					".')";

				lua_pushlstring( state, message.data( ), message.size( ) );
				lua_error( state );
			}
		}

		// The payload is plain data by contract: no functions, no
		// userdata, no cycles. Encoding enforces it rather than documenting it.
		auto payload = std::string{ "{}" };

		if ( lua_type( state, 2 ) == LUA_TTABLE ) {
			auto rendered = json_from_lua( state, 2 );

			if ( !rendered ) {
				const auto message = std::string{ "mcode.emit: the payload is not plain data: " } +
					rendered.error( ).msg;

				lua_pushlstring( state, message.data( ), message.size( ) );
				lua_error( state );
			}

			payload = *rendered;
		}

		if ( auto emitted = self->hooks_->emit( name, payload ); !emitted ) {
			lua_pushlstring( state, emitted.error( ).msg.data( ), emitted.error( ).msg.size( ) );
			lua_error( state );
		}

		return 0;
	}

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

		auto lua_skill_list( lua_State* state ) -> int {
			return surface_from( state )->handle_skill_list( state );
		}

		auto lua_mcp_register( lua_State* state ) -> int {
			return surface_from( state )->handle_mcp_register( state );
		}

	}

	auto api_surface::install( const install_request& request ) -> status {
		auto& host = request.host;

		host_ = &request.host;
		registry_ = &request.registry;
		providers_ = &request.providers;
		hooks_ = &request.hooks;
		manifest_ = request.details;
		skills_ = request.skills;
		servers_ = request.servers;

		// Identity first: `mcode.ext.name` is read by tools and by the log prefix,
		// and the definition file declares it as a field rather than a call.
		if ( auto name = host.set_global_string( EXTENSION_NAME_PATH, request.details.name );
			!name ) {
			return name;
		}

		if ( auto api_version = host.set_global_number( API_VERSION_PATH,
			static_cast< double >( API_VERSION ) ); !api_version ) {
			return api_version;
		}

		const struct {
			const char* path;
			lua_CFunction function;
		} ENTRIES[] = {
			{ "tool.register", lua_register_tool },
			{ "tool.unregister", lua_unregister_tool },
			{ "model.register", lua_register_provider },
			{ "on", lua_on },
			{ "off", lua_off },
			{ "emit", lua_emit },
			{ "log.debug", lua_log_debug },
			{ "log.info", lua_log_info },
			{ "log.warn", lua_log_warn },
			{ "log.error", lua_log_error },
			{ "skill.read", lua_skill_read },
			{ "skill.list", lua_skill_list },
			{ "mcp.register", lua_mcp_register },
		};

		for ( const auto& entry : ENTRIES ) {
			if ( auto registered = host.register_raw_function( entry.path, entry.function, this );
				!registered ) {
				return registered;
			}
		}

		return { };
	}

}
