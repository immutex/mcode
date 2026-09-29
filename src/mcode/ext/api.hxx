#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/skills/discovery.hxx"
#include "mcode/ext/api_mcp.hxx"

namespace mcode::ext {

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

		// The discovered skills this surface was installed with. Null when
		// discovery did not run.
		[[nodiscard]] auto installed_skills( ) const noexcept
			-> const mcode::skills::discovery_report* {
			return skills_;
		auto handle_mcp_register( lua_State* state ) -> int;

		// The store extension-declared servers land in. Null before install.
		[[nodiscard]] auto installed_servers( ) const noexcept -> mcp_server_store* {
			return servers_;
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

		// A COPY, not a pointer. The loader builds a manifest per candidate and
		// destroys it at the end of the iteration, so a pointer here dangles the
		// moment loading moves on -- and the surface is still live, reading
		// `manifest_.name` on every `mcode.log.*` call. A manifest is a handful of
		// small strings; owning one per extension costs nothing and removes the
		// aliasing entirely.
		manifest manifest_;

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
