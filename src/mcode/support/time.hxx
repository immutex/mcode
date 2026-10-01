#pragma once

#include <cstdint>

namespace mcode::support {

	// Milliseconds since the Unix epoch, from the system clock.
	//
	// The system clock, not the steady one: this value is stamped onto events that
	// are written to disk and correlated with other tools, so it has to be
	// comparable across processes and machines. Elapsed-time measurement is a
	// different question and uses steady_clock directly.
	[[nodiscard]] auto epoch_milliseconds( ) noexcept -> std::int64_t;

	// Milliseconds from a monotonic clock, for measuring elapsed time.
	//
	// Distinct from `epoch_milliseconds` on purpose: this one is only
	// meaningful as a difference, so it is never written to disk or compared
	// across processes. Every elapsed-time measurement in the harness reads it,
	// so two layers cannot disagree about how long something took.
	[[nodiscard]] auto monotonic_milliseconds( ) noexcept -> std::uint64_t;

}
