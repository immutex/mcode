#include "mcode/ext/api.hxx"
#include "mcode/ext/api_cfg.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/api_context.hxx"
#include "mcode/ext/api_fs.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/api_net.hxx"
#include "mcode/ext/api_timer.hxx"
#include "mcode/ext/defer.hxx"
#include "mcode/ext/notify.hxx"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <optional>
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

		constexpr auto EXTENSION_NAME_PATH = "ext.name";
		constexpr auto API_VERSION_PATH = "api_version";

		constexpr auto MAX_LOGGED_ARGUMENTS = 8;
		constexpr auto MAX_LOGGED_LENGTH = 2048;

		// an unrecognised class prompts: the read default auto-approves inside the workspace.
		[[nodiscard]] auto declared_tool_class( const std::string_view declared )
			-> std::optional< tool_class > {
			if ( declared == "read" ) {
				return tool_class::read;
			}

			if ( declared == "write" ) {
				return tool_class::write;
			}

			if ( declared == "exec" ) {
				return tool_class::exec;
			}

			if ( declared == "net" ) {
				return tool_class::net;
			}

			if ( declared == "spawn" ) {
				return tool_class::spawn;
			}

			return std::nullopt;
		}

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

		lua_getfield( state, definition, "schema" );

		if ( lua_type( state, -1 ) != LUA_TTABLE ) {
			lua_pop( state, 1 );

			lua_pushliteral( state, "mcode.tool.register: 'schema' must be a table" );
			lua_error( state );
		}

		auto schema = std::string{ };
		{
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
		definition_value.klass = declared_tool_class(
			read_field_string( state, definition, "permission" ) ).value_or( tool_class::exec );
		definition_value.source = tool_source::user_extension;
		definition_value.owner = tool.owner;
		definition_value.schema_json = std::move( schema );

		if ( auto added = self->registry_->add( std::move( definition_value ) ); !added ) {
			lua_unref( state, tool.function_reference );

			const auto message = std::string{ "mcode.tool.register: " } + added.error( ).msg;

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		tool.identifier = self->next_tool_identifier_++;

		self->by_name_.insert_or_assign( tool.name, self->tools_.size( ) );
		self->tools_.push_back( std::move( tool ) );

		lua_pushnumber( state, static_cast< double >( self->tools_.back( ).identifier ) );

		return 1;
	}

	auto api_surface::handle_unregister( lua_State* state ) -> int {
		auto* self = surface_from( state );

		const auto number = luaL_checknumber( state, 1 );
		const auto identifier = static_cast< std::int64_t >( number );

		if ( static_cast< double >( identifier ) != number || identifier <= 0 ) {
			lua_pushliteral( state, "mcode.tool.unregister: unknown id" );
			lua_error( state );
		}

		// By identifier, never by position: `register` returns a handle the
		// extension keeps, and a positional one named a different tool once any
		// earlier unregister shifted the vector.
		const auto found = std::find_if( self->tools_.begin( ), self->tools_.end( ),
			[ identifier ]( const registered_tool& candidate ) {
				return candidate.identifier == identifier;
			} );

		if ( found == self->tools_.end( ) ) {
			lua_pushliteral( state, "mcode.tool.unregister: unknown id" );
			lua_error( state );
		}

		const auto index = static_cast< std::size_t >( std::distance( self->tools_.begin( ), found ) );
		const auto& tool = *found;

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

		// stderr, not the session log: extension text must not enter the transcript.
		std::fprintf( stderr, "%s\n", message.c_str( ) );

		return 0;
	}

	auto api_surface::handle_register_provider( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.model.register expects a descriptor table" );
			lua_error( state );
		}

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

		if ( !manifest_allows( *self, "net" ) ) {
			const auto message = std::string{ "mcode.model.register: extension '" } +
				self->manifest_.name + "' declares no 'net' permission";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		const auto host = url_host( descriptor->endpoint );

		if ( host.empty( ) || !host_declared( *self, host ) ) {
			const auto message = std::string{ "mcode.model.register: extension '" } +
				self->manifest_.name + "' may not reach '" +
				( host.empty( ) ? descriptor->endpoint : host ) +
				"'; declare it as a 'net:<host>' permission";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		if ( descriptor->auth.from == model::auth_spec::source::environment &&
			!credential_declared( *self, descriptor->auth.name ) ) {
			const auto message = std::string{ "mcode.model.register: extension '" } +
				self->manifest_.name + "' may not read the credential '" +
				descriptor->auth.name + "'; declare it as a 'credential:<NAME>' permission";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		if ( auto added = self->providers_->add( std::move( *descriptor ),
			self->manifest_.name ); !added ) {
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

		timers_ = std::make_unique< timer_registry >( timer_registry::actions{
			.fire = [ this ]( const int reference ) { fire_timer( reference ); },
			.release = [ this ]( const int reference ) { release_timer( reference ); } } );

		if ( request.files != nullptr ) {
			auto context = std::make_unique< tools::tool_context >( );
			context->space = request.files;
			context->reads = request.file_reads;
			context->permissions = request.file_permissions;
			context->run_id = "ext-" + request.details.name;
			context->headless = true;

			file_context_ = std::move( context );
		}

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
