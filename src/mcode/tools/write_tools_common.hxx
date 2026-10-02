#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "mcode/tools/file_tools.hxx"

namespace mcode::tools {

	// must exceed read_file's 8 MiB cap, or a large file reads fine and every write is refused
	inline constexpr std::uintmax_t VERIFY_READ_BYTES = MAX_WRITE_FILE_BYTES;

	[[nodiscard]] auto raw_content_hash( const std::filesystem::path& absolute )
		-> result< std::string >;

	// the file must have been read this session and its recorded hash must still match disk
	[[nodiscard]] auto check_readable_state( session_reads& reads,
		const std::filesystem::path& absolute, const std::string_view path )
		-> result< std::string >;

	[[nodiscard]] auto refuse_protected( const std::string_view path ) -> std::string;

	// outside-workspace writes prompt (deny headless); deny rules and protected paths still deny
	[[nodiscard]] auto check_write_permission( tool_context& context,
		const std::string_view path ) -> std::optional< std::string >;

	// strips CR for comparison only; the file's own endings are restored on write
	[[nodiscard]] auto normalise( std::string_view text ) -> std::string;

}
