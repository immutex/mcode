#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/fs/workspace.hxx"
#include "mcode/tui/palette.hxx"

namespace mcode::tui {

	// The picker's row cap. Bounded, because the rows share the live region
	// with the prompt and the transcript.
	inline constexpr std::size_t MENTION_LIMIT = 8;

	// Paths the one walk collects before ranking. The walk is the shared
	// `workspace::glob`, so this bounds its cost, not the result.
	inline constexpr std::size_t MENTION_SCAN_LIMIT = 4000;

	// The fragment after an '@' that opens a mention, or nullopt when the
	// input has none. A space closes one, and the '@' must start a word, so
	// an address like name@host is not a mention.
	[[nodiscard]] auto mention_query( std::string_view input )
		-> std::optional< std::string_view >;

	// `input` with the open mention replaced by `@path `, so the picker
	// inserts a path the model can read rather than the file's contents.
	[[nodiscard]] auto mention_completion( std::string_view input, std::string_view path )
		-> std::string;

	// Ranks `paths` against a case-insensitive subsequence query: a path that
	// starts with it first, then a basename that starts with it, then any
	// subsequence. Registration order breaks ties, and the result is capped
	// at `limit`. An empty query keeps the list as it came in.
	[[nodiscard]] auto rank_mentions( const std::vector< std::string >& paths,
		std::string_view query, std::size_t limit ) -> std::vector< std::string >;

	// The workspace's files as forward-slashed relative paths. One walk
	// through `workspace::glob`, dropping the directory names the glob tool
	// always skips, so artifacts do not crowd out the result.
	[[nodiscard]] auto list_workspace_files( const mcode::workspace& space,
		std::size_t limit ) -> std::vector< std::string >;

	// The picker's file list, walked once and reused for every keystroke.
	// A null space is an empty picker, not an error.
	class mention_index {
	public:
		explicit mention_index( const mcode::workspace* space ) : space_( space ) { }

		// The palette rows for `query`, already ranked and capped.
		[[nodiscard]] auto matches( std::string_view query ) const
			-> std::vector< slash_command >;

	private:
		auto load( ) const -> void;

		const mcode::workspace* space_ = nullptr;
		mutable std::vector< std::string > files_;
		mutable bool loaded_ = false;
	};

}
