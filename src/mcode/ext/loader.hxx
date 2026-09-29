#pragma once

#include <chrono>
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
#include "mcode/events/bus.hxx"
#include "mcode/ext/api.hxx"
#include "mcode/skills/discovery.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/manifest.hxx"

namespace mcode::ext {

	struct load_outcome {
		std::string name;
		std::filesystem::path directory;

		// Tools the extension registered, by name.
		std::vector< std::string > tools;

		std::uint64_t bytes_used = 0;
	};

	struct load_failure {
		std::string name;
		std::filesystem::path directory;
		std::string reason;
	};

	struct load_report {
		std::vector< load_outcome > loaded;
		std::vector< load_failure > failed;

		// Extensions skipped because they are disabled by config or by
		// --no-extensions. Counted so "disabled" is never confused with "absent".
		std::size_t disabled = 0;

		[[nodiscard]] auto total_tools( ) const noexcept -> std::size_t;
	};

	// Owns everything a loaded extension needs to stay callable: the VM, the
	// installed API surface, and the tool closures that surface holds.
	//
	// The VM is not optional. A registry entry without a live VM is a tool the
	// model can see and cannot call, which is worse than an absent tool.
	struct loaded_extension {
		manifest details;
		std::unique_ptr< lua_host > host;
		std::unique_ptr< api_surface > surface;

		[[nodiscard]] auto tool_names( ) const -> std::vector< std::string >;
	};

	// The whole outcome of a load: what happened, and what stays alive.
	struct load_result {
		load_report report;
		std::vector< loaded_extension > extensions;

		// Tool name -> the surface that owns it, built once at load. Dispatch is
		// per model tool call, so resolving the owner by scanning every extension
		// and every tool in each one is a cost that grows with the square of the
		// extension count.
		std::map< std::string, api_surface*, std::less<> > tool_owners;

		[[nodiscard]] auto find( std::string_view name ) const -> const loaded_extension*;

		// Calls a tool by name on whichever extension registered it.
		[[nodiscard]] auto invoke( std::string_view tool_name, std::string_view arguments_json )
			-> result< std::string >;
	};


	// Everything an installer is handed for one extension. A struct rather than
	// five positional parameters, and it is the whole environment: there is no
	// sixth thing a caller has to remember to thread through.
	//
	// The surface object is owned by the caller: it holds the tool closures by
	// registry reference, so it must outlive the VM it was installed into.
	struct registration {
		lua_host& host;
		api_surface& surface;
		model::provider_registry& providers;
		hook_registry& hooks;
		const manifest& details;
	};

	struct loader_options {
		// `--no-extensions`. Disabling everything must leave a working, less
		// capable agent -- never a broken one.
		bool disabled = false;

		// Names to skip, from the config-level disable list.
		std::vector< std::string > disabled_names;

		std::uint64_t memory_limit_bytes = 0;
		std::chrono::milliseconds time_limit{ 0 };

		// Where the core API surface comes from. Injected so a test can load an
		// extension without the real host functions, and so the loader never
		// depends on the agent.
		std::function< status( const registration& ) > register_api;
	};

	// Discovers, validates, and loads extensions from the given roots.
	//
	// A failing extension is skipped and reported; it never fails the session.
	// Registration is expected to be cheap: an extension that does real work at
	// load time is a bug, and the report makes it visible as a slow load.
	// The tool registry is NOT a parameter: it arrives through
	// `options.register_api`, which is the single place that decides where a
	// registered tool lands. Passing it here as well threaded the same reference
	// twice and left one copy unread.
	//
	// Providers and hooks ARE parameters. The caller inspects them afterwards, and
	// the API surface holds pointers into them for the lifetime of every loaded
	// extension -- so the loader must not own them.
	[[nodiscard]] auto load_extensions( const std::vector< std::filesystem::path >& roots,
		model::provider_registry& providers, hook_registry& hooks,
		const loader_options& options = { } ) -> load_result;

	// The real API installer: everything the frozen surface defines. Passed as
	// `loader_options::register_api`, and injectable so a test can substitute a
	// narrower surface without the loader growing a second code path.
	// Captures the tool registry, which the surface writes into directly, and
	// the discovered skills, which `mcode.skill.read` resolves against. The
	// skills pointer must outlive every loaded extension.
	//
	// The MCP server store, when given, receives every server an extension
	// declares through `mcode.mcp.register`. The host process owns it and must
	// outlive every loaded extension, for the same reason the skills report does.
	//
	// Providers and hooks arrive per call, because the caller owns them and a
	// second reference would be a second source of truth.
	[[nodiscard]] auto default_register_api( tool_registry& registry,
		const skills::discovery_report* skills = nullptr,
		mcp_server_store* servers = nullptr )
		-> std::function< status( const registration& ) >;

	// The extension roots, in precedence order: project, then user. Project
	// shadows user, so a repository can pin a version without touching the
	// machine-level install.
	[[nodiscard]] auto default_roots( const std::filesystem::path& workspace )
		-> std::vector< std::filesystem::path >;

	// Unloads one extension: drops its registry entries and discards its VM. The
	// caller must not hold anything the extension registered.
	[[nodiscard]] auto unload_extension( tool_registry& registry, std::string_view owner ) -> std::size_t;

}
