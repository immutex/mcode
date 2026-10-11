#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/subagent.hxx"
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

	// The body is text; a non-text content type is refused rather than mangled.
	struct fetch_result {
		int status = 0;
		std::string content_type;
		std::string body;
		bool truncated = false;
	};

	// a Luau function is a value in the VM, so the closure is held by registry reference.
	struct registered_tool {
		std::string name;
		std::string description;
		std::string owner;

		// never reused and never positional: a handle must keep naming the same tool.
		std::int64_t identifier = 0;

		int function_reference = 0;
	};

	class api_surface {
	public:
		api_surface( ) = default;

		api_surface( api_surface&& other ) noexcept = default;
		auto operator=( api_surface&& other ) noexcept -> api_surface& = default;

		api_surface( const api_surface& ) = delete;
		auto operator=( const api_surface& ) -> api_surface& = delete;

		// install must run before sealing: sealing is a one-way door.
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

			// Copied by `install`, never stored by reference, so the caller's copy is
			// free to be a temporary.
			const std::function< mcode::ext::session_state( ) >* session_state = nullptr;

			const std::function< result< fork_result >( std::uint64_t, const std::string& ) >*
				session_forker = nullptr;

			// the declared hosts and credentials come from the manifest, not from here.
			mcode::net::http_client* http_client = nullptr;
			const std::function< search_result( const std::string& ) >* web_searcher
				= nullptr;

			const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* notifier = nullptr;
			const std::function< void( const std::string&, const std::string& ) >*
				instruction_sink = nullptr;

			const std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
				std::string_view ) >* skill_sink = nullptr;

			// Runs a read-only subagent and returns its report. Core, because the
			// capability restriction and the budget carve are harness guarantees; the
			// `task` tool shell over it is a bundled extension.
			const std::function< result< subagent_report >( const mcode::subagent_spec& ) >*
				subagent_runner = nullptr;

			// Routes to the core `bash` handler: the same sandbox, the same permission
			// gate, the same PTY. An extension cannot spawn a process of its own, so
			// this is the only way it runs one, and it is gated on the `spawn`
			// manifest permission.
			const std::function< result< std::string >( const std::string& command,
				std::int64_t timeout_ms ) >* bash_runner = nullptr;

			// Writes the current plan into session state, where recitation reads it.
			// The plan is the loop's, not the extension's, so disabling the extension
			// that maintains it loses the `todo` tool, never the plan itself.
			const std::function< void( std::string_view ) >* plan_sink = nullptr;

			// Fetches a URL the MODEL asked for, not one the extension chose. An
			// extension's own call is bounded by its static `net:<host>` declarations;
			// this is gated per domain and filtered against private addresses.
			const std::function< result< fetch_result >( const std::string& url,
				std::int64_t timeout_ms ) >* fetch_runner = nullptr;
		};

		auto install( const install_request& request ) -> status;

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

		// fires every due timer of this extension; the host drives it once per loop step.
		auto pump_timers( ) -> void;

		// runs every `mcode.defer` callback queued since the last pump, on the loop
		// thread; the host drives it once per loop step beside the timers.
		auto pump_deferred( ) -> void;

		[[nodiscard]] auto notifier( ) const noexcept
			-> const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* {
			return notifier_.get( );
		}

		[[nodiscard]] auto deferred( ) noexcept -> std::vector< int >& {
			return deferred_;
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

		[[nodiscard]] auto subagent_runner( ) const noexcept
			-> const std::function< result< subagent_report >( const mcode::subagent_spec& ) >* {
			return subagent_runner_.get( );
		}

		[[nodiscard]] auto bash_runner( ) const noexcept
			-> const std::function< result< std::string >( const std::string&,
				std::int64_t ) >* {
			return bash_runner_.get( );
		}

		[[nodiscard]] auto plan_sink( ) const noexcept
			-> const std::function< void( std::string_view ) >* {
			return plan_sink_.get( );
		}

		[[nodiscard]] auto fetch_runner( ) const noexcept
			-> const std::function< result< fetch_result >( const std::string&,
				std::int64_t ) >* {
			return fetch_runner_.get( );
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

		mcode::net::http_client* http_client_ = nullptr;
		std::unique_ptr< std::function< search_result( const std::string& ) > >
			web_searcher_;

		std::unique_ptr< std::function< void( const std::string&, const std::string&,
			const std::string& ) > > notifier_;
		std::vector< int > deferred_;
		std::unique_ptr< std::function< void( const std::string&, const std::string& ) > >
			instruction_sink_;
		std::unique_ptr< std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
			std::string_view ) > > skill_sink_;
		std::unique_ptr< std::function< result< subagent_report >( const mcode::subagent_spec& ) > >
			subagent_runner_;
		std::unique_ptr< std::function< result< std::string >( const std::string&,
			std::int64_t ) > > bash_runner_;
		std::unique_ptr< std::function< void( std::string_view ) > > plan_sink_;
		std::unique_ptr< std::function< result< fetch_result >( const std::string&,
			std::int64_t ) > > fetch_runner_;

		auto fire_timer( int function_reference ) -> void;
		auto release_timer( int function_reference ) -> void;

		std::unique_ptr< tools::tool_context > file_context_;

		// a copy, not a pointer: the loader destroys the manifest it built per candidate.
		mcode::ext::manifest manifest_;

		std::vector< registered_tool > tools_;
		std::int64_t next_tool_identifier_ = 1;

		std::map< std::string, std::size_t, std::less<> > by_name_;
	};

}
