#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcode {
	class workspace;
}

// The walk and the regex guard behind `glob` and `grep`, split from the two
// handlers. This is the part that decides what a search may look at; the
// handlers are the part that renders what it found.
namespace mcode::tools::search_walk {

	// std::regex backtracks catastrophically, so the cap is per file, enforced mid-scan
	inline constexpr std::uintmax_t GREP_FILE_BYTES = 1u * 1024u * 1024u;

	// lines longer than this are skipped: matching inside them is where backtracking lives
	inline constexpr std::size_t GREP_LINE_BYTES = 8u * 1024u;

	// The line cap does not bound backtracking: `(a+)+$` against a full line
	// is exponential in its length. This is the wall-clock bound on one call.
	inline constexpr auto GREP_SCAN_BUDGET = std::chrono::milliseconds{ 2'000 };

	inline constexpr std::size_t GREP_CONTEXT_CHARS = 120;

	// candidates per possible match, plus a floor, so a narrow pattern still sees the tree
	inline constexpr std::size_t GLOB_BUDGET_PER_RESULT = 4;
	inline constexpr std::size_t GLOB_BUDGET_BASE = 256;

	// The most paths a single glob may ask the walk to enumerate. It is a
	// bound on the WORK, not on the result: `max_results` multiplies into the
	// walk budget, so an unbounded value overflows it to a small number and
	// the caller asking for more results gets fewer.
	inline constexpr std::int64_t MAX_GLOB_LIMIT = 100'000;

	// A quantifier applied to a group that contains a quantifier: `(a+)+`,
	// `(a*)*`, `(a+){2,}`, `([ab]+)*`. Those are the shapes whose match cost
	// is exponential in the input, and `std::regex` offers no way to bound a
	// single `regex_search`, so they are refused rather than run.
	[[nodiscard]] auto has_nested_quantifier( const std::string_view pattern ) -> bool;

	// the workspace walk is ignore-blind; these would burn the whole result budget on artifacts
	[[nodiscard]] auto always_skipped( const std::string_view name ) noexcept -> bool;

	// root .gitignore only: one pattern per line, # comments, trailing / = dir, * wildcards
	class ignore_rules {
	public:
		auto load( const std::filesystem::path& root ) -> void;

		// `relative` uses forward slashes with no leading or trailing slash.
		[[nodiscard]] auto matches( const std::string_view relative ) const noexcept -> bool;

		// `*` stays within one segment; a pattern with no `/` matches the file name anywhere
		[[nodiscard]] static auto glob_match( const std::string_view pattern,
			const std::string_view path ) noexcept -> bool;

	private:
		struct rule {
			std::string pattern;
			bool directory_only = false;
		};

		std::vector< rule > rules_;
	};

	[[nodiscard]] auto collect_paths( const workspace& space, const ignore_rules& rules,
		const std::size_t budget ) -> std::vector< std::string >;

	[[nodiscard]] auto glob_matches_pattern( const std::string_view path,
		const std::string_view pattern ) -> bool;

}
