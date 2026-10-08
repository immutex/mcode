#include "mcode/agent/repetition_guard.hxx"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace mcode::agent {

	namespace {

		// Length of the shortest period that repeats at the END of `text`, or 0.
		//
		// Only exact contiguity counts. A near-repeat is not a loop: prose
		// legitimately echoes itself, and every fuzzy signal tried in the prior
		// art false-triggered on long structured thinking.
		[[nodiscard]] auto exact_period( const std::string_view text,
			const std::size_t minimum_unit, const std::size_t minimum_copies )
			-> std::size_t {
			const auto required = minimum_unit * minimum_copies;

			if ( text.size( ) < required ) {
				return 0;
			}

			// The longest a unit can be is half the text; the shortest is the
			// declared floor. Smallest period first, so the tightest unit wins
			// and `repeated_characters` is not overstated.
			const auto longest = text.size( ) / minimum_copies;

			for ( auto unit = minimum_unit; unit <= longest; ++unit ) {
				const auto tail = text.substr( text.size( ) - unit );

				auto copies = std::size_t{ 1 };
				auto cursor = text.size( ) - unit;

				while ( cursor >= unit && text.substr( cursor - unit, unit ) == tail ) {
					++copies;
					cursor -= unit;
				}

				if ( copies >= minimum_copies ) {
					return unit;
				}
			}

			return 0;
		}

		[[nodiscard]] auto is_space( const unsigned char character ) -> bool {
			return character == ' ' || character == '\t' || character == '\n'
				|| character == '\r' || character == '\f' || character == '\v';
		}

	}

	auto normalize_for_comparison( const std::string_view text ) -> std::string {
		auto out = std::string{ };
		out.reserve( text.size( ) );

		auto pending_space = false;

		for ( const auto raw : text ) {
			const auto character = static_cast< unsigned char >( raw );

			if ( is_space( character ) ) {
				// Collapsed rather than dropped, so "a b" and "ab" stay distinct.
				pending_space = !out.empty( );

				continue;
			}

			if ( pending_space ) {
				out.push_back( ' ' );
				pending_space = false;
			}

			out.push_back( static_cast< char >( std::tolower( character ) ) );
		}

		return out;
	}

	auto periodic_tail_length( const std::string_view text, const std::size_t minimum_unit,
		const std::size_t minimum_copies ) -> std::size_t {
		return exact_period( text, minimum_unit, minimum_copies );
	}

	auto shingle_overlap( const std::string_view a, const std::string_view b ) -> double {
		// 5 characters is short enough to catch a reworded sentence and long
		// enough that common English does not dominate the set.
		constexpr auto SHINGLE = std::size_t{ 5 };

		if ( a.size( ) < SHINGLE || b.size( ) < SHINGLE ) {
			return 0.0;
		}

		auto theirs = std::unordered_set< std::string_view >{ };
		theirs.reserve( b.size( ) );

		for ( auto index = std::size_t{ 0 }; index + SHINGLE <= b.size( ); ++index ) {
			theirs.insert( b.substr( index, SHINGLE ) );
		}

		auto shared = std::size_t{ 0 };
		auto total = std::size_t{ 0 };

		for ( auto index = std::size_t{ 0 }; index + SHINGLE <= a.size( ); ++index ) {
			++total;

			if ( theirs.contains( a.substr( index, SHINGLE ) ) ) {
				++shared;
			}
		}

		if ( total == 0 ) {
			return 0.0;
		}

		return static_cast< double >( shared ) / static_cast< double >( total );
	}

	auto detect_repetition( const std::string_view text,
		const std::vector< std::string >& previous_turns ) -> repetition_verdict {
		auto verdict = repetition_verdict{ };

		if ( text.size( ) < REPETITION_MIN_UNIT * REPETITION_MIN_COPIES ) {
			return verdict;
		}

		if ( const auto unit = periodic_tail_length( text, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ); unit > 0 ) {
			verdict.looped = true;
			verdict.repeated_characters = unit;
			verdict.unit_sample = std::string{ text.substr( text.size( ) - unit ) };

			return verdict;
		}

		// The cross-turn form. Only the most recent turn is compared: a loop
		// re-states the last thing it said, and reaching further back would
		// match legitimately recurring phrasing.
		if ( previous_turns.empty( ) ) {
			return verdict;
		}

		const auto current = normalize_for_comparison( text );

		if ( current.size( ) < REPETITION_MIN_COMPARABLE ) {
			return verdict;
		}

		// Both sides normalized. Comparing a normalized string against a RAW one
		// made the signal dead: a verbatim repeat measured 0.885 against a 0.90
		// threshold, because the two sides differed by whitespace and case alone.
		const auto previous = normalize_for_comparison( previous_turns.back( ) );

		if ( previous.size( ) < REPETITION_MIN_COMPARABLE ) {
			return verdict;
		}

		const auto overlap = shingle_overlap( current, previous );

		if ( overlap >= REPETITION_OVERLAP_THRESHOLD ) {
			verdict.looped = true;
			verdict.repeated_characters = current.size( );
			verdict.unit_sample = current.substr( 0, REPETITION_MIN_UNIT );
		}

		return verdict;
	}

}
