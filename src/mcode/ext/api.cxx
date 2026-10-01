#include "mcode/ext/api.hxx"
#include "mcode/ext/api_cfg.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/api_context.hxx"
#include "mcode/ext/api_fs.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/api_net.hxx"
#include "mcode/ext/api_timer.hxx"
#include "mcode/ext/defer.hxx"
#include "mcode/ext/notify.hxx"

#include <chrono>
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

	auto api_surface::install( const install_request& request ) -> status {
		auto& host = request.host;

		host_ = &request.host;
		registry_ = &request.registry;
		providers_ = &request.providers;
		hooks_ = &request.hooks;
		manifest_ = request.details;
		skills_ = request.skills;
		servers_ = request.servers;
		config_ = request.config;
		commands_ = request.commands;

		if ( request.session_state ) {
			session_state_ = std::make_unique<
				std::function< mcode::ext::session_state( ) > >(
				*request.session_state );
		}

		if ( request.session_forker ) {
			session_forker_ = std::make_unique<
				std::function< result< mcode::ext::fork_result >( std::uint64_t,
					const std::string& ) > >( *request.session_forker );
		}

		// The manifest's declared net hosts, copied at install so the check
		// reads a snapshot rather than re-parsing the manifest per call.
		// Declared in the manifest as `net_hosts = ["host", ...]`.
		net_hosts_ = request.net_hosts;
		http_client_ = request.http_client;

		if ( request.web_searcher ) {
			web_searcher_ = std::make_unique<
				std::function< mcode::ext::search_result( const std::string& ) > >(
				*request.web_searcher );
		}

		if ( request.notifier ) {
			notifier_ = std::make_unique<
				std::function< void( const std::string&, const std::string&,
					const std::string& ) > >( *request.notifier );
		}

		if ( request.deferred ) {
			deferred_ = std::make_unique< std::vector< int > >( *request.deferred );
		}

		if ( request.instruction_sink ) {
			instruction_sink_ = std::make_unique<
				std::function< void( const std::string&, const std::string& ) > >(
				*request.instruction_sink );
		}

		if ( request.skill_sink ) {
			skill_sink_ = std::make_unique<
				std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
					std::string_view ) > >( *request.skill_sink );
		}

		// The timer pump runs the closures through this surface's own VM, on
		// whatever thread the host's loop pumps from. One registry per surface:
		// when the surface dies the timers die with it.
		timers_ = std::make_unique< timer_registry >(
			[ this ]( ) { pump_timers_once( ); } );

		// The file context is assembled here, from the borrowed pointers the
		// request carries, so `fs.read` / `fs.write` dispatch through the same
		// tool handlers the model's own calls use.
		if ( request.files != nullptr ) {
			auto context = std::make_unique< tools::tool_context >( );
			context->space = request.files;
			context->reads = request.file_reads;
			context->permissions = request.file_permissions;
			context->run_id = "ext-" + request.details.name;
			context->headless = true;

			file_context_ = std::move( context );
		}

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
			{ "skill.register", lua_skill_register },
			{ "mcp.register", lua_mcp_register },
			{ "fs.read", lua_fs_read },
			{ "fs.write", lua_fs_write },
			{ "cfg.get", lua_cfg_get },
			{ "cmd.register", lua_cmd_register },
			{ "timer.at", lua_timer_at },
			{ "timer.every", lua_timer_every },
			{ "session.snapshot", lua_session_snapshot },
			{ "session.fork", lua_session_fork },
			{ "defer", lua_defer },
			{ "notify", lua_notify },
			{ "net.get", lua_net_get },
			{ "net.search", lua_net_search },
			{ "context.add_instructions", lua_context_add_instructions },
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
