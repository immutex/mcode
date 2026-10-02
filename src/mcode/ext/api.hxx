#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/ext/api_mcp.hxx"
#include "mcode/ext/api_session.hxx"
#include "mcode/ext/api_timer.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/skills/discovery.hxx"
#include "mcode/support/config.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/session_reads.hxx"

namespace mcode::ext {

	class command_registry;
	class timer_registry;

	struct search_hit {
		std::string title;
		std::string url;
		std::string snippet;
	};

	using search_result = mcode::result< std::vector< search_hit > >;

	// a Luau function is a value in the VM, so the closure is held by registry reference.
	struct registered_tool {
		std::string name;
		std::string description;
		std::string owner;

		int function_reference = 0;
	};

	class api_surface {
	public:
		api_surface( ) = default;

		api_surface( api_surface&& other ) noexcept = default;
		auto operator=( api_surface&& other ) noexcept -> api_surface& = default;

		api_surface( const api_surface& ) = delete;
		auto operator=( const api_surface& ) -> api_surface& = delete;

		// installs before the host is sealed; sealing is a one-way door.
		struct install_request {
			lua_host& host;
			tool_registry& registry;
			model::provider_registry& providers;
			hook_registry& hooks;
			// qualified: the accessor name shadows the type in class scope (gcc -Wchanges-meaning).
			const mcode::ext::manifest& details;

			const mcode::skills::discovery_report* skills = nullptr;
			mcp_server_store* servers = nullptr;

			const mcode::config::merged_config* config = nullptr;

			workspace* files = nullptr;
			tools::session_reads* file_reads = nullptr;
			perm::permission_engine* file_permissions = nullptr;

			command_registry* commands = nullptr;

			std::function< mcode::ext::session_state( ) >* session_state = nullptr;

			std::function< result< fork_result >( std::uint64_t, const std::string& ) >*
				session_forker = nullptr;

			// an empty list permits nothing: fail-closed rather than absent.
			std::vector< std::string > net_hosts = { };
			mcode::net::http_client* http_client = nullptr;
			const std::function< search_result( const std::string& ) >* web_searcher
				= nullptr;

			const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* notifier = nullptr;
			std::vector< int >* deferred = nullptr;
			const std::function< void( const std::string&, const std::string& ) >*
				instruction_sink = nullptr;

			const std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
				std::string_view ) >* skill_sink = nullptr;
		};

		auto install( const install_request& request ) -> status;

		[[nodiscard]] auto providers( ) const noexcept -> const model::provider_registry& {
			return providers_ != nullptr ? *providers_ : empty_providers_;
		}

		// a raised error is a contract violation and becomes an error here, not an empty result.
		[[nodiscard]] auto invoke( std::string_view tool_name, std::string_view arguments_json )
			-> result< std::string >;

		[[nodiscard]] auto tools( ) const noexcept -> const std::vector< registered_tool >& {
			return tools_;
		}

		// releases the closures while the owning VM is alive; the registry entries go with them.
		auto release_all( lua_host& host ) -> void;

		// public for the C closures Luau calls; not part of the author-facing surface.
		auto handle_register( lua_State* state ) -> int;
		auto handle_unregister( lua_State* state ) -> int;
		auto handle_log( lua_State* state, const char* level ) -> int;
		auto handle_register_provider( lua_State* state ) -> int;
		auto handle_on( lua_State* state ) -> int;
		auto handle_off( lua_State* state ) -> int;
		auto handle_emit( lua_State* state ) -> int;
		auto handle_skill_read( lua_State* state ) -> int;
		auto handle_skill_list( lua_State* state ) -> int;
		auto handle_skill_register( lua_State* state ) -> int;

		[[nodiscard]] auto installed_skills( ) const noexcept
			-> const mcode::skills::discovery_report* {
			return skills_;
		}

		auto handle_mcp_register( lua_State* state ) -> int;

		[[nodiscard]] auto installed_servers( ) const noexcept -> mcp_server_store* {
			return servers_;
		}

		[[nodiscard]] auto config( ) const noexcept -> const mcode::config::merged_config* {
			return config_;
		}

		[[nodiscard]] auto commands( ) const noexcept -> command_registry* {
			return commands_;
		}

		[[nodiscard]] auto raw_host( ) const noexcept -> lua_host* {
			return host_;
		}

		[[nodiscard]] auto session_state( ) const noexcept
			-> const std::function< mcode::ext::session_state( ) >* {
			return session_state_.get( );
		}

		[[nodiscard]] auto session_forker( ) const noexcept
			-> const std::function< result< fork_result >( std::uint64_t, const std::string& ) >* {
			return session_forker_.get( );
		}

		[[nodiscard]] auto timers( ) const noexcept -> timer_registry* {
			return timers_.get( );
		}

		[[nodiscard]] auto net_hosts( ) const noexcept -> const std::vector< std::string >& {
			return net_hosts_;
		}

		[[nodiscard]] auto notifier( ) const noexcept
			-> const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* {
			return notifier_.get( );
		}

		[[nodiscard]] auto deferred( ) const noexcept -> std::vector< int >* {
			return deferred_.get( );
		}

		[[nodiscard]] auto instruction_sink( ) const noexcept
			-> const std::function< void( const std::string&, const std::string& ) >* {
			return instruction_sink_.get( );
		}

		[[nodiscard]] auto skill_sink( ) const noexcept
			-> const std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
				std::string_view ) >* {
			return skill_sink_.get( );
		}

		[[nodiscard]] auto http_client( ) const noexcept -> mcode::net::http_client* {
			return http_client_;
		}

		[[nodiscard]] auto web_searcher( ) const noexcept
			-> const std::function< search_result( const std::string& ) >* {
			return web_searcher_.get( );
		}

		// gated entries check this, never a session-wide setting.
		[[nodiscard]] auto manifest( ) const noexcept -> const mcode::ext::manifest& {
			return manifest_;
		}

		[[nodiscard]] auto file_context( ) const noexcept -> tools::tool_context* {
			return file_context_.get( );
		}

	private:

		lua_host* host_ = nullptr;
		tool_registry* registry_ = nullptr;
		model::provider_registry* providers_ = nullptr;
		hook_registry* hooks_ = nullptr;

		const mcode::skills::discovery_report* skills_ = nullptr;
		mcp_server_store* servers_ = nullptr;

		const mcode::config::merged_config* config_ = nullptr;

		command_registry* commands_ = nullptr;

		std::unique_ptr< std::function< mcode::ext::session_state( ) > > session_state_;
		std::unique_ptr< std::function< result< fork_result >( std::uint64_t, const std::string& ) > >
			session_forker_;

		std::unique_ptr< timer_registry > timers_;

		std::vector< std::string > net_hosts_;

		mcode::net::http_client* http_client_ = nullptr;
		std::unique_ptr< std::function< search_result( const std::string& ) > >
			web_searcher_;

		std::unique_ptr< std::function< void( const std::string&, const std::string&,
			const std::string& ) > > notifier_;
		std::unique_ptr< std::vector< int > > deferred_;
		std::unique_ptr< std::function< void( const std::string&, const std::string& ) > >
			instruction_sink_;
		std::unique_ptr< std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
			std::string_view ) > > skill_sink_;

		auto pump_timers_once( ) -> void;

		std::unique_ptr< tools::tool_context > file_context_;

		// a copy, not a pointer: the loader destroys the manifest it built per candidate.
		mcode::ext::manifest manifest_;

		static const model::provider_registry empty_providers_;

		std::vector< registered_tool > tools_;

		std::map< std::string, std::size_t, std::less<> > by_name_;
	};

}
