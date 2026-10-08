#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::agent {

	// Detection thresholds. Both signals are deliberately conservative: a false
	// positive discards a good answer and spends a retry, which is worse than
	// missing one loop.
	//
	// The reasoning is measured, not guessed. A shipped guard for another
	// harness (pi-repetition-guard) began with a two-stage design -- an early
	// "suspect" signal, then a harder abort -- and found that every
	// early-warning signal it tried (shingle novelty, block repeat, fractional
	// period) false-triggered on legitimate long structured thinking, while
	// exact contiguous repetition never did. The two-stage design was collapsed
	// into the single exact signal. This follows that result.

	// The shortest repeated unit that counts. Below this, an ordinary phrase
	// ("the file, the file") trips it.
	inline constexpr std::size_t REPETITION_MIN_UNIT = 30;

	// How many times the tail unit must repeat to fire. Two is the minimum that
	// cannot be a coincidence of prose.
	inline constexpr std::size_t REPETITION_MIN_COPIES = 2;

	// A steer is a whole extra turn, so the budget is small and hard. Exhausting
	// it falls through to the existing failure handling rather than looping.
	inline constexpr std::size_t MAX_REPETITION_STEERS = 2;

	// The corrective message. Two properties matter:
	//
	//   - It is action-oriented. The measured failure is the model rehearsing
	//     the next action without taking it ("let me check git status" repeated),
	//     so the steer tells it to stop describing and execute.
	//   - It is short. Every steer is appended to the context that caused the
	//     loop, so a long corrective prompt feeds the degradation it is meant to
	//     fix. This is 19 tokens.
	inline constexpr std::string_view REPETITION_STEER =
		"You repeated yourself without making progress. Stop describing the next "
		"step and do it now.";

	// What the guard decided about one assistant message.
	struct repetition_verdict {
		bool looped = false;

		// How many characters the repeated tail occupies, for the log and for
		// telling the model what it repeated.
		std::size_t repeated_characters = 0;

		// The shortest repeated unit, truncated for a diagnostic. Empty when
		// nothing fired.
		std::string unit_sample;
	};

	// The length of the shortest period the tail of `text` repeats with, or 0
	// when the tail is not periodic.
	//
	// Pure, so the thresholds can be tested against real prose rather than
	// tuned by feel. Scans only the tail, because a legitimate long answer may
	// contain a repeated quotation or code block in the middle; what identifies
	// a runaway is that the message never leaves the repeat.
	[[nodiscard]] auto periodic_tail_length( std::string_view text,
		std::size_t minimum_unit, std::size_t minimum_copies ) -> std::size_t;

	// Whether one assistant message is a repetition loop, considering its own
	// text and the text of the turns before it.
	//
	// Two signals, either sufficient:
	//
	//   1. The message's own tail is periodic (the tape loop).
	//   2. The message is a near-duplicate of the previous assistant message,
	//      which is the cross-turn form: the model rewords the same intent
	//      instead of advancing.
	[[nodiscard]] auto detect_repetition( std::string_view text,
		const std::vector< std::string >& previous_turns ) -> repetition_verdict;

	// Case-folded, whitespace-collapsed form used for the cross-turn comparison.
	// Exposed because the comparison's behaviour on punctuation and spacing is
	// worth testing directly.
	[[nodiscard]] auto normalize_for_comparison( std::string_view text ) -> std::string;

	// Fraction of `a`'s 5-gram shingles that also appear in `b`, in [0, 1].
	// Returns 0 when either side is too short to shingle.
	[[nodiscard]] auto shingle_overlap( std::string_view a, std::string_view b ) -> double;

	// The near-duplicate threshold for the cross-turn signal.
	inline constexpr double REPETITION_OVERLAP_THRESHOLD = 0.90;

	// A message shorter than this is never compared: a terse "Done." after a
	// terse "Done." is not a loop.
	inline constexpr std::size_t REPETITION_MIN_COMPARABLE = 120;

}
