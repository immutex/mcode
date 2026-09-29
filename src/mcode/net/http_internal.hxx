#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::net::detail {

	// HTTP/1.1 `Transfer-Encoding: chunked`.
	//
	// The SSE body is read straight off the socket, so the framing has to be
	// removed before the event parser sees it. Left in, a chunk-size line is a
	// line with no colon and is discarded -- but a chunk boundary landing inside
	// an event splits its JSON across two lines, the continuation is discarded as
	// well, and the truncated payload is rejected. Cloudflare fronts the gateway
	// and always chunks, so this is the normal case, not an edge case.
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
