#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::ext {

	inline constexpr std::int64_t API_VERSION = 1;

	struct manifest {
		std::string name;
		std::string version;
		std::int64_t api_version = 0;
		std::string description;
		std::vector< std::string > permissions;


		[[nodiscard]] auto has_permission( std::string_view permission ) const noexcept -> bool;

		// derived from permissions; only the `net:`/`credential:` prefixed entries, unfolded.
		[[nodiscard]] auto net_hosts( ) const -> std::vector< std::string_view >;
		[[nodiscard]] auto credential_names( ) const -> std::vector< std::string_view >;
	};

	// unknown keys are rejected: a typo that disables a capability beats a load error.
	[[nodiscard]] auto load_manifest( const std::filesystem::path& directory ) -> result< manifest >;

	[[nodiscard]] auto known_permissions( ) -> const std::vector< std::string_view >&;

}
