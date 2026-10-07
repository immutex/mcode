#pragma once

// Shared by snapshot.cxx and snapshot_restore.cxx; not part of the store's public surface.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/fs/snapshot.hxx"

namespace mcode::snapshot_detail {

	// A single capture above this is refused rather than held in memory: the loop logs the
	// refusal and the edit still runs, so the bound costs an undo, never the edit.
	inline constexpr std::uintmax_t MAX_SNAPSHOT_FILE_BYTES = 32u * 1024u * 1024u;

	inline constexpr std::string_view TEMPORARY_SUFFIX = ".mcode-tmp";

	// The blob name is the workspace's own content hash, so the store does not invent a second
	// convention. A hash is not a security boundary here; what makes a weak hash safe is the
	// re-hash on read, which turns a collision or a corruption into a loud error rather than a
	// silently wrong restore.
	[[nodiscard]] auto content_address( std::string_view bytes ) -> std::string;

	[[nodiscard]] auto read_whole_file( const std::filesystem::path& path )
		-> result< std::string >;

	// One object per capture, so the index replays line by line and a torn tail is visible.
	[[nodiscard]] auto entry_line( const snapshot_store::index_entry& entry ) -> std::string;

}
