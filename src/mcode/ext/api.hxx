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

	// `session_state` and `fork_result` come from `api_session.hxx`, included
	// above; the surface stores the callables the host installs and the entry
	// points marshal them.

	// One web-search result, as `net.search` returns it. Defined here so the
	// surface can hold the searcher callable without including the provider
	// layer.
	struct search_hit {
		std::string title;
		std::string url;
		std::string snippet;
	};

	using search_result = mcode::result< std::vector< search_hit > >;

	// A tool an extension registered, plus the VM that owns it.
	//
	// The handler is NOT a std::function copied out of the VM. Luau functions are
	// values in a lua_State that the host keeps alive by registry reference, so
	// the handler holds the reference and the host, and the call happens through
	// the VM. Copying the function into C++ is not possible and pretending
	// otherwise is how a harness ends up with a callback that outlives its VM.
	struct registered_tool {
		std::string name;
		std::string description;
		std::string owner;

		// The `run` closure, by registry reference.
		int function_reference = 0;
	};

	// The `mcode` API a first-party or third-party extension sees. Owns the
	// tool closures and translates a call into a VM invocation.
	//
	// Scoped to one extension: the API functions must reject a tool registered by
	// a different extension, and attribution has to come from the VM that made
	// the call rather than from whatever the caller claims.
	class api_surface {
	public:
		api_surface( ) = default;

		api_surface( api_surface&& other ) noexcept = default;
		auto operator=( api_surface&& other ) noexcept -> api_surface& = default;

		api_surface( const api_surface& ) = delete;
		auto operator=( const api_surface& ) -> api_surface& = delete;

		// Installs the whole frozen surface into a host, before it is sealed.
		// `registry` receives the tool definitions, `providers` the declared model
		// providers, and `manifest` supplies identity and the permission set the
		// gated entries check.
		// The five collaborators travel together: they are the whole environment an
		// extension is installed into, and a caller always has all five.
		struct install_request {
			lua_host& host;
			tool_registry& registry;
			model::provider_registry& providers;
			hook_registry& hooks;
			const manifest& details;

			// The discovered skills, for `mcode.skill.read` / `mcode.skill.list`.
			// Null when discovery did not run; the entries then read as empty.
			const mcode::skills::discovery_report* skills = nullptr;
			// Where `mcode.mcp.register` puts a declared server. Null when the
			// host built no store; the entry then reports so rather than dropping
			// the declaration.
			mcp_server_store* servers = nullptr;

			// The merged configuration, for `mcode.cfg.get`. Null when the host
			// installed none; the entry then answers every key with the default.
			const mcode::config::merged_config* config = nullptr;

			// The workspace the extension's `fs.*` entries resolve against.
			// Null when the host installed no file context: `fs.read` and
			// `fs.write` then report so rather than resolving against nothing.
			workspace* files = nullptr;
			tools::session_reads* file_reads = nullptr;
			perm::permission_engine* file_permissions = nullptr;

			// The slash-command registry, for `mcode.cmd.register`. LAST in the
			// initializer order on purpose: the host that would own one is the
			// REPL, which this slice does not own, so no caller names it yet and
			// the loader's initializer compiles unchanged. Null means the entry
			// exists but refuses loudly -- `cmd.register` is implemented but
			// non-functional until a host installs a registry.
			command_registry* commands = nullptr;

			// The session state, for `mcode.session.snapshot`. A function, not a
			// struct: the loop's sequence moves under the surface, so the copy
			// must be taken at call time. Null when the host installed none; the
			// entry then reports so rather than inventing state.
			std::function< mcode::ext::session_state( ) >* session_state = nullptr;

			// The branch operation, for `mcode.session.fork`. Null when the host
			// installed none; the entry then reports so rather than forking
			// nothing.
			std::function< result< fork_result >( std::uint64_t, const std::string& ) >*
				session_forker = nullptr;

			// The hosts this manifest declares for `net.get`, and the client it
			// sends through. An empty list permits nothing; a null client is
			// reported. The searcher, when given, backs `net.search`.
			// Defaulted so a caller that installs no network context can omit
			// it; an empty list permits nothing, which is the fail-closed
			// answer rather than an absent one.
			std::vector< std::string > net_hosts = { };
			mcode::net::http_client* http_client = nullptr;
			const std::function< search_result( const std::string& ) >* web_searcher
				= nullptr;

			// `mcode.notify`, `mcode.defer` and `mcode.context.add_instructions`
			// route through these. All null-tolerant: notify falls back to an
			// attributed stderr line, the others refuse loudly.
			const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* notifier = nullptr;
			std::vector< int >* deferred = nullptr;
			const std::function< void( const std::string&, const std::string& ) >*
				instruction_sink = nullptr;

			// The sink `mcode.skill.register` registers through. Null when the
			// host installed none; the entry then refuses loudly rather than
			// silently dropping the registration.
			const std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
				std::string_view ) >* skill_sink = nullptr;
		};

		auto install( const install_request& request ) -> status;

		[[nodiscard]] auto providers( ) const noexcept -> const model::provider_registry& {
			return providers_ != nullptr ? *providers_ : empty_providers_;
		}

		// Calls a registered tool by name with a JSON argument object, returning
		// the extension's result as JSON.
		//
		// The extension's `run` returns `value, err`: a string result, or
		// nil and a message. A raised error is a contract violation and becomes an
		// error here rather than a silent empty result.
		[[nodiscard]] auto invoke( std::string_view tool_name, std::string_view arguments_json )
			-> result< std::string >;

		[[nodiscard]] auto tools( ) const noexcept -> const std::vector< registered_tool >& {
			return tools_;
		}

		// Unregisters every tool this surface registered and releases its closure.
		//
		// Called when an extension failed to finish loading: the VM that owns the
		// closures is about to be destroyed, so each reference must be dropped while
		// the VM is still alive -- and each registry entry must go with it, or the
		// model sees a tool that can never run.
		auto release_all( lua_host& host ) -> void;

		// Tools the extension registered but that the host could not accept --
		// a name collision, a bad schema. Reported rather than dropped.

		// The raw entry points. Public because the C closures that Luau calls are
		// free functions and cannot be friends of every instantiation; they are not
		// part of the author-facing surface.
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

		// The discovered skills this surface was installed with. Null when
		// discovery did not run.
		[[nodiscard]] auto installed_skills( ) const noexcept
			-> const mcode::skills::discovery_report* {
			return skills_;
		}

		auto handle_mcp_register( lua_State* state ) -> int;

		// The store extension-declared servers land in. Null before install.
		[[nodiscard]] auto installed_servers( ) const noexcept -> mcp_server_store* {
			return servers_;
		}

		// The merged configuration the surface reads `[extensions.<name>]`
		// through. Null when the host installed none; `cfg.get` then answers
		// every key with the caller's default rather than guessing.
		[[nodiscard]] auto config( ) const noexcept -> const mcode::config::merged_config* {
			return config_;
		}

		// The slash-command registry every loaded surface registers into. Null
		// before install; `cmd.register` then refuses rather than dropping.
		[[nodiscard]] auto commands( ) const noexcept -> command_registry* {
			return commands_;
		}

		// The VM this surface is installed into. Null before install; the
		// command dispatch path resolves the thread through it.
		[[nodiscard]] auto raw_host( ) const noexcept -> lua_host* {
			return host_;
		}

		// The session-state provider, for `mcode.session.snapshot`. Null when
		// the host installed none.
		[[nodiscard]] auto session_state( ) const noexcept
			-> const std::function< mcode::ext::session_state( ) >* {
			return session_state_.get( );
		}

		// The branch operation, for `mcode.session.fork`. Null when the host
		// installed none.
		[[nodiscard]] auto session_forker( ) const noexcept
			-> const std::function< result< fork_result >( std::uint64_t, const std::string& ) >* {
			return session_forker_.get( );
		}

		// The timer registry this extension scheduled into. Null when the host
		// installed none; `timer.at` / `timer.every` then refuse rather than
		// scheduling into nothing.
		[[nodiscard]] auto timers( ) const noexcept -> timer_registry* {
			return timers_.get( );
		}

		// The hosts this extension's manifest declares for `net.get`. Empty
		// means the extension may reach nothing, which is fail-closed.
		[[nodiscard]] auto net_hosts( ) const noexcept -> const std::vector< std::string >& {
			return net_hosts_;
		}

		// The notifier `mcode.notify` delivers through. Null when the host
		// installed none; the entry then falls back to an attributed stderr line.
		[[nodiscard]] auto notifier( ) const noexcept
			-> const std::function< void( const std::string&, const std::string&,
				const std::string& ) >* {
			return notifier_.get( );
		}

		// The deferred queue `mcode.defer` appends to. Null when the host
		// installed none; the entry then refuses.
		[[nodiscard]] auto deferred( ) const noexcept -> std::vector< int >* {
			return deferred_.get( );
		}

		// The instruction sink `mcode.context.add_instructions` contributes
		// through. Null when the host installed none; the entry then reports so.
		[[nodiscard]] auto instruction_sink( ) const noexcept
			-> const std::function< void( const std::string&, const std::string& ) >* {
			return instruction_sink_.get( );
		}

		// The skill sink `mcode.skill.register` registers through. Null when the
		// host installed none; the entry then refuses rather than dropping.
		[[nodiscard]] auto skill_sink( ) const noexcept
			-> const std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
				std::string_view ) >* {
			return skill_sink_.get( );
		}

		// The HTTP client `net.get` sends through. Null when the host installed
		// none; the entry then reports so rather than building a second client.
		[[nodiscard]] auto http_client( ) const noexcept -> mcode::net::http_client* {
			return http_client_;
		}

		// The search provider `net.search` routes through. Null when the host
		// installed none.
		[[nodiscard]] auto web_searcher( ) const noexcept
			-> const std::function< search_result( const std::string& ) >* {
			return web_searcher_.get( );
		}

		// The calling extension's own manifest. The gated entries check their
		// permission against this, never against a session-wide setting.
		// The return type is qualified: the accessor's own name shadows the
		// struct inside the class scope.
		[[nodiscard]] auto manifest( ) const noexcept -> const mcode::ext::manifest& {
			return manifest_;
		}

		// The file-tool context `fs.read` / `fs.write` dispatch through. Null
		// when the host installed no workspace; the entries then report so
		// rather than resolving paths against nothing.
		[[nodiscard]] auto file_context( ) const noexcept -> tools::tool_context* {
			return file_context_.get( );
		}

	private:

		lua_host* host_ = nullptr;
		tool_registry* registry_ = nullptr;
		model::provider_registry* providers_ = nullptr;
		hook_registry* hooks_ = nullptr;

		// Borrowed, not owned: discovery happens once per session before the
		// extensions load, and the report outlives every surface that reads it.
		const mcode::skills::discovery_report* skills_ = nullptr;
		// Borrowed, not owned: the host process owns the store, and every loaded
		// surface writes declared servers into it.
		mcp_server_store* servers_ = nullptr;

		// Borrowed, not owned: the merged config outlives every surface that
		// reads it, exactly as the skills report does.
		const mcode::config::merged_config* config_ = nullptr;

		// Borrowed, not owned: the host process owns the command registry and
		// every loaded surface registers into it.
		command_registry* commands_ = nullptr;

		// Owned: the providers are per-extension callables the install assembles
		// from the request, and nothing outside the surface calls them.
		std::unique_ptr< std::function< mcode::ext::session_state( ) > > session_state_;
		std::unique_ptr< std::function< result< fork_result >( std::uint64_t, const std::string& ) > >
			session_forker_;

		// Owned: one timer registry per surface. When the surface dies with its
		// extension, the registry dies with it, so no timer can fire after its
		// VM is gone.
		std::unique_ptr< timer_registry > timers_;

		// The manifest's declared net hosts, copied at install so the check
		// reads a snapshot rather than re-parsing the manifest per call.
		std::vector< std::string > net_hosts_;

		// Borrowed, not owned: the host process owns the HTTP client and the
		// search provider; the surface only routes through them.
		mcode::net::http_client* http_client_ = nullptr;
		std::unique_ptr< std::function< search_result( const std::string& ) > >
			web_searcher_;

		// Owned per-surface callables and state: the notifier, the deferred
		// closure queue, and the instruction sink. The deferred queue holds
		// registry references in the owning VM and dies with the surface.
		std::unique_ptr< std::function< void( const std::string&, const std::string&,
			const std::string& ) > > notifier_;
		std::unique_ptr< std::vector< int > > deferred_;
		std::unique_ptr< std::function< void( const std::string&, const std::string& ) > >
			instruction_sink_;
		std::unique_ptr< std::function< result< std::uint64_t >( const mcode::skills::skill_entry&,
			std::string_view ) > > skill_sink_;

		// Fires the timers that are due, through this surface's VM. The
		// registry's pump callback lands here.
		auto pump_timers_once( ) -> void;

		// Owned: the context is one struct of borrowed pointers assembled per
		// extension, and nothing outside the surface needs it.
		std::unique_ptr< tools::tool_context > file_context_;

		// A COPY, not a pointer. The loader builds a manifest per candidate and
		// destroys it at the end of the iteration, so a pointer here dangles the
		// moment loading moves on -- and the surface is still live, reading
		// `manifest_.name` on every `mcode.log.*` call. A manifest is a handful of
		// small strings; owning one per extension costs nothing and removes the
		// aliasing entirely.
		mcode::ext::manifest manifest_;

		// Returned by `providers()` before install. Static so the accessor needs no
		// branch on a nullable member in its return type.
		static const model::provider_registry empty_providers_;

		std::vector< registered_tool > tools_;

		// Name -> index into tools_. A map, not a vector: this is the tool-dispatch
		// path, which runs once per model tool call, and a linear scan there is a
		// cost the registry beside it does not pay.
		std::map< std::string, std::size_t, std::less<> > by_name_;
	};

}
