#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/model/provider.hxx"

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
		tool_class klass = tool_class::read;

		// The `run` closure, by registry reference.
		int function_reference = 0;

		// Schema as JSON, pre-rendered. Rendered once at registration because
		// docs/23 warns that computing it per turn invalidates the prompt cache.
		std::string schema_json;

		lua_host* host = nullptr;
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
		auto install( lua_host& host, tool_registry& registry, model::provider_registry& providers,
			hook_registry& hooks, const manifest& manifest ) -> status;

		[[nodiscard]] auto providers( ) const noexcept -> const model::provider_registry& {
			return providers_ != nullptr ? *providers_ : empty_providers_;
		}

		// Calls a registered tool by name with a JSON argument object, returning
		// the extension's result as JSON.
		//
		// The extension's `run` returns `value, err` (docs/18): a string result, or
		// nil and a message. A raised error is a contract violation and becomes an
		// error here rather than a silent empty result.
		[[nodiscard]] auto invoke( std::string_view tool_name, std::string_view arguments_json )
			-> result< std::string >;

		[[nodiscard]] auto tools( ) const noexcept -> const std::vector< registered_tool >& {
			return tools_;
		}

		// Tools the extension registered but that the host could not accept --
		// a name collision, a bad schema. Reported rather than dropped.
		[[nodiscard]] auto refusals( ) const noexcept -> const std::vector< std::string >& {
			return refusals_;
		}

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

	private:

		lua_host* host_ = nullptr;
		tool_registry* registry_ = nullptr;
		model::provider_registry* providers_ = nullptr;
		hook_registry* hooks_ = nullptr;
		const manifest* manifest_ = nullptr;

		// Returned by `providers()` before install. Static so the accessor needs no
		// branch on a nullable member in its return type.
		static const model::provider_registry empty_providers_;

		std::vector< registered_tool > tools_;
		std::vector< std::string > refusals_;

		// Name -> index into tools_. Kept beside the vector so a lookup on the
		// dispatch path does not walk it.
		std::vector< std::pair< std::string, std::size_t > > by_name_;
	};

}
