#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::net {

	struct sse_event {
		std::string event;
		std::string data;
		std::string id;
		std::string retry;
	};

	class sse_parser {
	public:
		using event_callback = std::function< void( sse_event&& ) >;

		inline static constexpr std::size_t MAX_LINE_BYTES = 1u * 1024u * 1024u;
		inline static constexpr std::size_t MAX_EVENT_BYTES = 8u * 1024u * 1024u;

		explicit sse_parser( event_callback on_event ) : on_event_( std::move( on_event ) ) { }

		auto feed( std::string_view chunk ) -> void;
		auto finish( ) -> void;

		[[nodiscard]] auto events_parsed( ) const noexcept -> std::size_t { return events_parsed_; }

	private:
		auto process_line( std::string_view line ) -> void;
		auto dispatch( ) -> void;

		event_callback on_event_;
		std::string buffer_;
		sse_event current_;
		bool saw_data_ = false;
		std::size_t events_parsed_ = 0;
	};

	[[nodiscard]] auto extract_delta_text( std::string_view data_payload ) -> result< std::string >;

}
