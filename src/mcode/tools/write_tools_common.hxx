#pragma once

// Internal helpers shared by write_tools.cxx (write) and
// write_tools_edit.cxx (edit). Not part of the public tool surface.

#include <optional>
#include <string>
#include <string_view>

#include "mcode/tools/file_tools.hxx"

namespace mcode::tools {

	// Files up to the write cap are legitimate edit targets, so the verifier
	// must hash raw bytes past read_file's 8 MiB whole-file cap — otherwise a
	// 9 MiB file reads fine and then every write is refused.
	inline constexpr std::uintmax_t VERIFY_READ_BYTES = MAX_WRITE_FILE_BYTES;

	[[nodiscard]] auto raw_content_hash( const std::filesystem::path& absolute )
		-> result< std::string >;

	// The invariant check shared by write-overwrite and edit: the file must have
	// been read this session, and the hash recorded then must still match disk.
	[[nodiscard]] auto check_readable_state( session_reads& reads,
		const std::filesystem::path& absolute, const std::string_view path )
		-> result< std::string >;

	[[nodiscard]] auto refuse_protected( const std::string_view path ) -> std::string;

	// The write decision, shared by write and edit. The engine resolves
	// it: a deny rule denies, the default set prompts for a path outside
	// the workspace (or denies headless), and an answer is honoured.
	// Protected paths keep their deny semantics through the engine's
	// floor, so a refusal here is still a refusal.
	[[nodiscard]] auto check_write_permission( tool_context& context,
		const std::string_view path ) -> std::optional< std::string >;

	// Strips CR for comparison while keeping LF structure; the file's own
	// endings are restored before writing.
	[[nodiscard]] auto normalise( std::string_view text ) -> std::string;

}
