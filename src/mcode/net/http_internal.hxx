#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::net::detail {

	// The SSE body comes straight off the socket, so framing must be removed before the parser.
	class chunked_decoder {
	public:
		auto feed( std::string_view raw ) -> status;

		[[nodiscard]] auto take_decoded( ) -> std::string;
		[[nodiscard]] auto finished( ) const -> bool;

	private:
		enum class state { size, data };

		state state_ = state::size;
		std::string pending_;
		std::string decoded_;
		std::uint64_t remaining_ = 0;
		bool done_ = false;
	};

	[[nodiscard]] auto is_chunked( std::string_view transfer_encoding ) -> bool;

}
