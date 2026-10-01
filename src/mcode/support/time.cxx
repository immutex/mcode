#include "mcode/support/time.hxx"

#include <chrono>

namespace mcode::support {

	auto epoch_milliseconds( ) noexcept -> std::int64_t {
		return std::chrono::duration_cast< std::chrono::milliseconds >(
			std::chrono::system_clock::now( ).time_since_epoch( ) ).count( );
	}

	auto monotonic_milliseconds( ) noexcept -> std::uint64_t {
		return static_cast< std::uint64_t >(
			std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( ) );
	}

}
