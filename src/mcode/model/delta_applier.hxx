#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	// Applies a provider descriptor to a stream of SSE events, producing
	// canonical chat_events.
	//
	// The applier is stateful because tool-call fragments are: a provider splits
	// a call's arguments across arbitrary events, keyed by index, and only the
	// concatenation is JSON. Holding that state here -- in C++, in one place --
	// is what lets the descriptor stay a pure mapping with no per-provider code.
	class delta_applier {
	public:
		explicit delta_applier( const provider_descriptor& descriptor );
		explicit delta_applier( provider_descriptor&& descriptor );

		// Feeds one SSE event. Returns the canonical events it produced, which may
		// be empty (most events carry one field, and a keep-alive carries none).
		//
		// A malformed data payload is an error, not a silent skip: a gateway that
		// changes shape mid-stream must surface, not produce a truncated turn.
		[[nodiscard]] auto feed( std::string_view event_name, std::string_view data )
			-> result< std::vector< chat_event > >;

		// Emits the completed tool calls and a turn_done. Called when the stream
		// ends, including on a truncated one -- whatever fragments arrived are
		// reported so the caller can see what was lost.
		[[nodiscard]] auto finish( ) -> std::vector< chat_event >;

		[[nodiscard]] auto saw_terminal_event( ) const noexcept -> bool { return terminal_seen_; }
		[[nodiscard]] auto accumulated_usage( ) const noexcept -> const usage& { return usage_; }

	private:
		struct pending_call {
			int index = 0;
			std::string id;
			std::string name;
			std::string args_fragments;
			bool emitted = false;
		};

		auto pending_for( const int index ) -> pending_call&;

		// Held by value. A reference would make `delta_applier{ load_descriptor() }`
		// silently dangle, and a descriptor is small, set once at registration, and
		// never mutated during a stream -- so ownership is the boring correct choice
		// and removes a lifetime footgun from the API.
		provider_descriptor descriptor_;
		std::vector< pending_call > pending_;
		usage usage_;
		std::string stop_reason_;
		bool terminal_seen_ = false;
	};

}
