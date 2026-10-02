#pragma once

#include <cstdint>

namespace mcode::support {

	// system clock, not steady: stamped onto events correlated across machines.
	[[nodiscard]] auto epoch_milliseconds( ) noexcept -> std::int64_t;

	// monotonic, so only meaningful as a difference; never written to disk.
	[[nodiscard]] auto monotonic_milliseconds( ) noexcept -> std::uint64_t;

}
