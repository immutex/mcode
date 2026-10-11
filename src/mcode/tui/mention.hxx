#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/fs/workspace.hxx"
#include "mcode/tui/palette.hxx"

namespace mcode::tui {

	inline constexpr std::size_t MENTION_LIMIT = 8;

	inline constexpr std::size_t MENTION_SCAN_LIMIT = 4000;

	// The fragment after an '@', or nullopt when the input has none. A space
	// closes one, and the '@' must start a word, so name@host is not a mention.
	[[nodiscard]] auto mention_query( std::string_view input )
		-> std::optional< std::string_view >;

	// Replaces the open mention with `@path `, so the model reads a path and
	// not the file's contents.
	[[nodiscard]] auto mention_completion( std::string_view input, std::string_view path )
		-> std::string;

	// Ranks against a case-insensitive subsequence query: a path prefix first,
	// then a basename prefix, then any subsequence; ties keep registration order.
	[[nodiscard]] auto rank_mentions( const std::vector< std::string >& paths,
		std::string_view query, std::size_t limit ) -> std::vector< std::string >;

	[[nodiscard]] auto list_workspace_files( const mcode::workspace& space,
		std::size_t limit ) -> std::vector< std::string >;

	// Walked once and reused for every keystroke; a null space is an empty
	// picker, not an error.
	class mention_index {
	public:
		explicit mention_index( const mcode::workspace* space ) : space_( space ) { }

		[[nodiscard]] auto matches( std::string_view query ) const
			-> std::vector< slash_command >;

	private:
		auto load( ) const -> void;

		const mcode::workspace* space_ = nullptr;
		mutable std::vector< std::string > files_;
		mutable bool loaded_ = false;
	};

}
