#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::config {

	// Config scopes, lowest precedence first.
	//
	// The rule that matters: **project scope can never widen.** It may add `ask`
	// or `deny`, and nothing else. A cloned repository that could set `allow` or
	// disable a sandbox would be a one-clone compromise, which is the same defect
	// as auto-loading repo extensions (`12`).
	enum class scope {
		managed,
		user,
		project,
		session,
	};

	[[nodiscard]] constexpr auto to_string( const scope value ) noexcept -> std::string_view {
		switch ( value ) {
			case scope::managed: return "managed";
			case scope::user: return "user";
			case scope::project: return "project";
			case scope::session: return "session";
		}

		return "user";
	}

	// Keys a project-scope file may set. Everything else in a project file is
	// rejected at load rather than ignored, because a silently dropped
	// security-relevant key is the failure mode this list exists to prevent.
	inline constexpr auto PROJECT_WRITABLE_PREFIXES = std::array{
		std::string_view{ "permissions.deny" },
		std::string_view{ "permissions.ask" },
	};

	// The top-level sections a config file may define. A key outside these is a
	// typo, and storing it silently means the user believes a setting took effect
	// when it did not -- which is the failure the parser exists to prevent.
	//
	// Section granularity, not per-key: the nested keys are owned by the docs that
	// define them, and a per-key list here would be a second copy that drifts.
	inline constexpr auto CONFIG_SECTIONS = std::array{
		std::string_view{ "model" },
		std::string_view{ "agent" },
		std::string_view{ "context" },
		std::string_view{ "sandbox" },
		std::string_view{ "ui" },
		std::string_view{ "extensions" },
		std::string_view{ "permissions" },
	};

	struct layer {
		scope level = scope::user;
		std::filesystem::path origin;
		toml::table values;
	};

	class merged_config {
	public:
		// Layers are applied lowest-precedence first. A later layer overrides a
		// scalar; arrays merge with later entries appended.
		[[nodiscard]] static auto merge( std::vector< layer > layers ) -> result< merged_config >;

		[[nodiscard]] auto get_string( std::string_view key ) const -> std::optional< std::string >;
		[[nodiscard]] auto get_int( std::string_view key ) const -> std::optional< std::int64_t >;
		[[nodiscard]] auto get_bool( std::string_view key ) const -> std::optional< bool >;
		[[nodiscard]] auto get_string_array( std::string_view key ) const
			-> std::vector< std::string >;

		// Which layer supplied a key. Used by `mcode config show` to answer "why is
		// this value what it is", which is the question that makes scope layering
		// debuggable rather than mysterious.
		[[nodiscard]] auto source_of( std::string_view key ) const -> std::optional< scope >;

		[[nodiscard]] auto keys( ) const noexcept -> const std::map< std::string, toml::value, std::less<> >& {
			return values_;
		}

		[[nodiscard]] auto loaded_scopes( ) const noexcept -> const std::vector< scope >& {
			return loaded_;
		}

	private:
		std::map< std::string, toml::value, std::less<> > values_;
		std::map< std::string, scope, std::less<> > origins_;
		std::vector< scope > loaded_;
	};

	// Standard scope paths, per platform.
	[[nodiscard]] auto default_layer_paths( ) -> std::vector< std::pair< scope, std::filesystem::path > >;

	// Loads and merges the standard scopes. A missing file is skipped; an
	// unreadable or malformed one is an error, because a config that half-loads
	// is worse than one that fails.
	[[nodiscard]] auto load( const std::filesystem::path& project_root = { } )
		-> result< merged_config >;

	// Parses one file as a given scope, enforcing the project-scope restriction.
	[[nodiscard]] auto load_layer( scope level, const std::filesystem::path& path )
		-> result< layer >;

}
