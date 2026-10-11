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
#include "mcode/ext/trust_store.hxx"
#include "mcode/support/config.hxx"
#include "mcode/tools/session_reads.hxx"

namespace mcode::ext {

	enum class load_trust {
		// The bundled directory beside the binary: shipped with it, gated by the installer.
		trusted_by_location,
		granted,
		// A project extension with no grant. It is read and hashed, never executed.
		ungranted,
		denied,
		// Granted, but the bytes do not match the grant. Never auto-regranted.
		changed,
	};

	[[nodiscard]] auto to_string( load_trust value ) noexcept -> std::string_view;

	struct load_outcome {
		std::string name;
		std::filesystem::path directory;
		std::vector< std::string > tools;

		// copied out of the surface: the report does not expose live surfaces.
		std::string version;
		std::string description;

		std::string tree_sha256;
		load_trust trust = load_trust::trusted_by_location;

		std::uint64_t bytes_used = 0;
	};

	struct load_failure {
		std::string name;
		std::filesystem::path directory;
		std::string reason;
	};

	// not a failure: an extension that did nothing must still be visible in the report.
	struct load_inert {
		std::string name;
		std::filesystem::path directory;
		load_trust reason = load_trust::ungranted;
		std::string detail;
	};

	struct load_report {
		std::vector< load_outcome > loaded;
		std::vector< load_failure > failed;
		std::vector< load_inert > inert;

		std::size_t disabled = 0;

		[[nodiscard]] auto total_tools( ) const noexcept -> std::size_t;
	};

	// the VM is not optional: a registry entry without one is a tool the model cannot call.
	struct loaded_extension {
		manifest details;
		std::unique_ptr< lua_host > host;
		std::unique_ptr< api_surface > surface;

		[[nodiscard]] auto tool_names( ) const -> std::vector< std::string >;
	};

	struct load_result {
		load_report report;
		std::vector< loaded_extension > extensions;

		std::map< std::string, api_surface*, std::less<> > tool_owners;

		[[nodiscard]] auto find( std::string_view name ) const -> const loaded_extension*;

		[[nodiscard]] auto invoke( std::string_view tool_name, std::string_view arguments_json )
			-> result< std::string >;

		// fires every loaded extension's due timers; the host drives it once per loop step.
		auto pump_timers( ) -> void;
	};


	// the surface is owned by the caller and must outlive its VM.
	struct registration {
		lua_host& host;
		api_surface& surface;
		model::provider_registry& providers;
		hook_registry& hooks;
		const manifest& details;
	};

	struct loader_options {
		bool disabled = false;

		std::vector< std::string > disabled_names;

		std::uint64_t memory_limit_bytes = 0;
		std::chrono::milliseconds time_limit = DEFAULT_TIME_LIMIT;

		// The workspace's own extension root. A candidate beneath it is a project extension
		// and is inert until a grant matches its tree digest; everything outside it (the
		// user and bundled roots) is trusted by location; empty means no project scope.
		std::filesystem::path project_root;

		// Defaults to the machine trust store when null.
		const trust_store* trust = nullptr;

		std::function< status( const registration& ) > register_api;
	};

	// a failing extension is skipped and reported; it never fails the session.
	[[nodiscard]] auto load_extensions( const std::vector< std::filesystem::path >& roots,
		model::provider_registry& providers, hook_registry& hooks,
		const loader_options& options = { } ) -> load_result;

	// loads the machine trust store and gates the workspace's project root with it.
	[[nodiscard]] auto load_workspace_extensions( const std::filesystem::path& workspace,
		const std::vector< std::filesystem::path >& roots,
		model::provider_registry& providers, hook_registry& hooks,
		const loader_options& options = { } ) -> load_result;

	// all borrowed: must outlive every loaded extension.
	struct register_context {
		const skills::discovery_report* skills = nullptr;
		mcp_server_store* servers = nullptr;
		const mcode::config::merged_config* config = nullptr;
		mcode::workspace* space = nullptr;
		tools::session_reads* reads = nullptr;
		perm::permission_engine* permissions = nullptr;
	};

	[[nodiscard]] auto default_register_api( tool_registry& registry,
		const register_context& context = { } )
		-> std::function< status( const registration& ) >;

	// project before user: project shadows user.
	[[nodiscard]] auto default_roots( const std::filesystem::path& workspace )
		-> std::vector< std::filesystem::path >;

	// the only root whose contents arrived with the repository, so the only one needing a grant.
	[[nodiscard]] auto project_extension_root( const std::filesystem::path& workspace )
		-> std::filesystem::path;

	[[nodiscard]] auto unload_extension( tool_registry& registry,
		std::string_view owner ) -> std::size_t;

}
