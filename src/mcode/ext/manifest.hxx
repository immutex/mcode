#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::ext {

	// The frozen API version this build provides. An extension declares a floor
	// and the loader compares two integers (docs/25, C3).
	inline constexpr std::int64_t API_VERSION = 1;

	// The manifest, frozen at v1 (docs/19, C3).
	struct manifest {
		std::string name;
		std::string version;
		std::int64_t api_version = 0;
		std::string description;
		std::vector< std::string > permissions;

		std::filesystem::path directory;

		[[nodiscard]] auto has_permission( std::string_view permission ) const noexcept -> bool;
	};

	// Parses and validates `ext.toml`. Unknown keys are rejected, because a typo
	// that silently disables a capability is worse than a load error.
	[[nodiscard]] auto load_manifest( const std::filesystem::path& directory ) -> result< manifest >;

	// The permission set from docs/18 §Capability model.
	[[nodiscard]] auto known_permissions( ) -> const std::vector< std::string_view >&;

}
