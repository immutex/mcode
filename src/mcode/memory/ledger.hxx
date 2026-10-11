#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::memory {

	// A hard cap: past it the write is refused, and the error names the ledger to prune.
	inline constexpr std::size_t MAX_NOTES_LINES = 200;

	// Never inside the workspace: a ledger the repository ships would be untrusted text.
	[[nodiscard]] auto ledger_path( const std::filesystem::path& workspace )
		-> result< std::filesystem::path >;

	// Appends the text verbatim: no model is in this path.
	[[nodiscard]] auto append_note( const std::filesystem::path& workspace,
		std::string_view text, std::string_view source ) -> status;

	[[nodiscard]] auto read_ledger( const std::filesystem::path& workspace )
		-> result< std::string >;

	[[nodiscard]] auto line_count( std::string_view text ) noexcept -> std::size_t;

}
