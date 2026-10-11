#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::agent {

	// Below this an ordinary phrase ("the file, the file") trips the guard.
	inline constexpr std::size_t REPETITION_MIN_UNIT = 30;

	// Two is the minimum that cannot be a coincidence of prose.
	inline constexpr std::size_t REPETITION_MIN_COPIES = 2;

	// A steer is a whole extra turn; exhausting the budget falls through to the
	// existing failure handling rather than looping.
	inline constexpr std::size_t MAX_REPETITION_STEERS = 2;

	// Short by design: the steer is appended to the context that caused the loop,
	// so a long corrective prompt feeds the degradation it exists to fix.
	inline constexpr std::string_view REPETITION_STEER =
		"You repeated yourself without making progress. Stop describing the next "
		"step and do it now.";

	struct repetition_verdict {
		bool looped = false;
		std::size_t repeated_characters = 0;
		std::string unit_sample;
	};

	// Scans only the tail: a legitimate long answer may quote or repeat code in
	// the middle, while a runaway never leaves the repeat.
	[[nodiscard]] auto periodic_tail_length( std::string_view text,
		std::size_t minimum_unit, std::size_t minimum_copies ) -> std::size_t;

	// Either signal suffices: the message's own tail is periodic, or it is a
	// near-duplicate of the previous assistant message -- the model rewording the
	// same intent instead of advancing.
	[[nodiscard]] auto detect_repetition( std::string_view text,
		const std::vector< std::string >& previous_turns ) -> repetition_verdict;

	[[nodiscard]] auto normalize_for_comparison( std::string_view text ) -> std::string;

	// Fraction of `a`'s 5-gram shingles that also appear in `b`, in [0, 1]. Zero
	// when either side is too short to shingle.
	[[nodiscard]] auto shingle_overlap( std::string_view a, std::string_view b ) -> double;

	inline constexpr double REPETITION_OVERLAP_THRESHOLD = 0.90;

	// A terse "Done." after a terse "Done." is not a loop.
	inline constexpr std::size_t REPETITION_MIN_COMPARABLE = 120;

}
