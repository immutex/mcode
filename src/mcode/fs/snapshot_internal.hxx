#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/fs/snapshot.hxx"

namespace mcode::snapshot_detail {

	// above this a capture is refused rather than held in memory; the edit still runs.
	inline constexpr std::uintmax_t MAX_SNAPSHOT_FILE_BYTES = 32u * 1024u * 1024u;

	inline constexpr std::string_view TEMPORARY_SUFFIX = ".mcode-tmp";

	// A hash is not a security boundary here: what makes a weak one safe is the re-hash on
	// read, which turns a collision into a loud error rather than a silently wrong restore.
	[[nodiscard]] auto content_address( std::string_view bytes ) -> std::string;

	[[nodiscard]] auto read_whole_file( const std::filesystem::path& path )
		-> result< std::string >;

	// One object per capture, so the index replays line by line and a torn tail is visible.
	[[nodiscard]] auto entry_line( const snapshot_store::index_entry& entry ) -> std::string;

}
