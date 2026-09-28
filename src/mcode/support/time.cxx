#include "mcode/support/time.hxx"

#include <chrono>

namespace mcode::support {

	auto epoch_milliseconds( ) noexcept -> std::int64_t {
		return std::chrono::duration_cast< std::chrono::milliseconds >(
			std::chrono::system_clock::now( ).time_since_epoch( ) ).count( );
	}

}
