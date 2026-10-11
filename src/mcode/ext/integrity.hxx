#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::ext {

	inline constexpr std::uintmax_t MAX_EXTENSION_FILE_BYTES = 4u * 1024u * 1024u;
	inline constexpr std::uintmax_t MAX_EXTENSION_TREE_BYTES = 32u * 1024u * 1024u;
	inline constexpr std::size_t MAX_EXTENSION_FILES = 4096;

	struct snapshot_entry {
		std::string relative_path;
		std::string sha256;
		std::string bytes;
	};

	// The bytes that were hashed are the bytes that get compiled. Every file is
	// read exactly once, so there is no second read and therefore no TOCTOU
	// window between the trust check and execution.
	struct extension_snapshot {
		std::string tree_sha256;
		std::vector< snapshot_entry > files;

		[[nodiscard]] auto find( std::string_view relative_path ) const -> const std::string*;
	};

	[[nodiscard]] auto sha256_hex( std::string_view bytes ) -> std::string;

	// Refuses symlinks, escapes, and anything over the ceilings; never truncates.
	[[nodiscard]] auto snapshot_extension( const std::filesystem::path& root )
		-> result< extension_snapshot >;

	// Resolves a require target as the loader always has: bare, `.luau`, or `/init.luau`.
	[[nodiscard]] auto resolve_module( const extension_snapshot& snapshot,
		std::string_view module_path ) -> const std::string*;

}
