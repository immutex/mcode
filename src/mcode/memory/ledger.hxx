#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::memory {

	// A hard cap, enforced at write time. Past it the write is refused and the
	// oldest entries are named, because the human is the writer and is the only
	// actor who can judge what is still true.
	inline constexpr std::size_t MAX_NOTES_LINES = 200;

	// One file, no index, no database. At the scale the evidence supports --
	// tens of entries, one developer, one repo -- a single file is greppable,
	// diffable and auditable by reading it, and an index buys nothing that `grep`
	// does not while costing a dependency slot.
	[[nodiscard]] auto ledger_path( const std::filesystem::path& workspace )
		-> result< std::filesystem::path >;

	// Appends the user's text verbatim under a dated heading. No model is in this
	// path: an LLM write pass costs 24-94% more tokens for no measurable quality
	// change, and its output is where the measured form degradation comes from.
	[[nodiscard]] auto append_note( const std::filesystem::path& workspace,
		std::string_view text, std::string_view source ) -> status;

	[[nodiscard]] auto read_ledger( const std::filesystem::path& workspace )
		-> result< std::string >;

	[[nodiscard]] auto line_count( std::string_view text ) noexcept -> std::size_t;

}
