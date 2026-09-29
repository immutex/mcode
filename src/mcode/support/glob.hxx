#pragma once

#include <string_view>
#include <vector>

namespace mcode::support {

	// Splits a slash-separated pattern or path into segments. Empty segments
	// are dropped, so a doubled slash or a trailing slash changes nothing --
	// the same path spelled two ways is one pattern.
	[[nodiscard]] auto glob_segments( std::string_view text )
		-> std::vector< std::string_view >;

	// Single-segment wildcard match: `*` runs within the segment, `?` stands
	// for one character. No slash handling -- a pattern or path must already
	// be segment-split for `**` to mean anything.
	[[nodiscard]] auto wildcard_match( std::string_view pattern,
		std::string_view text ) -> bool;

	// Segment-wise glob with `**` crossing directories: `**` consumes zero or
	// more segments, every other segment matches exactly one through
	// wildcard_match. An empty pattern matches nothing.
	[[nodiscard]] auto glob_match( std::string_view pattern,
		std::string_view path ) -> bool;

}
