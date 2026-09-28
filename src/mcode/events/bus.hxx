#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::events {

	// A closed tagged union, not std::variant.
	//
	// The argument is serialization stability, not speed: `kind` is the stable
	// integer the session log keys on, and an open hierarchy would make the log
	// schema implicit. At 100-1000 events/sec the representation is unmeasurable.
	//
	// Order matters: the numeric values are the log's primary key, so a new kind
	// is appended and an existing one is never renumbered or removed.
	enum class kind : std::uint16_t {
		session_start = 0,
		session_end = 1,
		turn_start = 2,
		turn_end = 3,
		step_start = 4,
		step_end = 5,
		assistant_delta = 6,
		tool_pre_call = 7,
		tool_call = 8,
		tool_result = 9,
		spawn_pre = 10,
		prompt_pre = 11,
		extension_loaded = 12,
		extension_error = 13,
		compaction = 14,
		error = 15,
	};

	inline constexpr std::size_t KIND_COUNT = 16;

	[[nodiscard]] constexpr auto to_string( const kind value ) noexcept -> std::string_view {
		switch ( value ) {
			case kind::session_start: return "session.start";
			case kind::session_end: return "session.end";
			case kind::turn_start: return "turn.start";
			case kind::turn_end: return "turn.end";
			case kind::step_start: return "step.start";
			case kind::step_end: return "step.end";
			case kind::assistant_delta: return "assistant.delta";
			case kind::tool_pre_call: return "tool.pre_call";
			case kind::tool_call: return "tool.call";
			case kind::tool_result: return "tool.result";
			case kind::spawn_pre: return "spawn.pre";
			case kind::prompt_pre: return "prompt.pre";
			case kind::extension_loaded: return "extension.loaded";
			case kind::extension_error: return "extension.error";
			case kind::compaction: return "context.compaction";
			case kind::error: return "session.error";
		}

		return "unknown";
	}

	// Events drained per publish before the bus gives up and reports it.
	//
	// A handler that publishes on every event it receives would otherwise spin
	// forever: the pending deque is drained in a loop, and each drain appends more
	// work. The cap makes that a counted failure rather than a hang.
	inline constexpr std::size_t MAX_EVENTS_PER_PUBLISH = 10'000;

	// The three kinds that accept a veto. Everything else is notification-only,
	// so a handler cannot accidentally block a turn by returning a value.
	[[nodiscard]] constexpr auto is_vetoable( const kind value ) noexcept -> bool {
		return value == kind::tool_pre_call || value == kind::spawn_pre ||
			value == kind::prompt_pre;
	}

	struct event {
		kind type = kind::session_start;

		// Monotonic; the log's primary key and checkpoint index.
		std::uint64_t sequence = 0;
		std::int64_t timestamp_ms = 0;

		// Owned, not borrowed. Producers run on worker threads and the loop
		// consumes later, so no view into producer memory ever crosses a thread
		// boundary. At these rates the copy is unmeasurable, and the alternative
		// requires proving a buffer is never resized while any event referencing it
		// is alive.
		std::string payload_json;
	};

	struct veto {
		std::string reason;
		std::string source;
	};

	class bus {
	public:
		using subscription_id = std::uint64_t;

		using handler = std::function< void( const event& ) >;

		// A veto handler returns a veto to block, or nothing to allow.
		using veto_handler = std::function< std::optional< veto >( const event& ) >;

		bus( ) = default;

		// Subscribe before publishing, and pass the event by const reference.
		// Constructing a std::function per publish is the cost this avoids.
		auto subscribe( kind type, handler function ) -> subscription_id;
		auto subscribe_veto( kind type, veto_handler function ) -> subscription_id;

		// O(1): swap-remove plus a tombstone, so unsubscribing mid-dispatch cannot
		// invalidate iteration.
		auto unsubscribe( subscription_id id ) -> void;

		// Synchronous, loop thread only.
		//
		// Returns the veto when a vetoable kind was blocked, so the caller can
		// surface the reason. A non-vetoable kind never returns a veto.
		auto publish( event value ) -> std::optional< veto >;

		// Events emitted from inside a handler land here and are drained after the
		// current dispatch completes, so nesting is flat rather than recursive.
		[[nodiscard]] auto pending_count( ) const noexcept -> std::size_t {
			return pending_.size( );
		}

		// Events dropped because a single publish exceeded
		// MAX_EVENTS_PER_PUBLISH. Non-zero means a handler is publishing in
		// response to its own output.
		[[nodiscard]] auto overflow_drops( ) const noexcept -> std::uint64_t {
			return overflow_drops_;
		}

		// True while a dispatch is in progress. A nested publish is queued rather
		// than recursed, so there is no depth to report -- only this flag.
		[[nodiscard]] auto dispatching( ) const noexcept -> bool { return dispatching_; }

		[[nodiscard]] auto subscriber_count( kind type ) const noexcept -> std::size_t;

		// Handlers detached after this many consecutive failures. The bus catches a
		// throwing handler rather than letting it abort dispatch, and counts it.
		[[nodiscard]] auto handler_failures( ) const noexcept -> std::uint64_t {
			return handler_failures_;
		}

	private:
		struct entry {
			subscription_id id = 0;
			handler function;
			veto_handler veto_function;
			bool alive = true;
		};

		auto drain_pending( ) -> void;
		auto dispatch_one( const event& value ) -> std::optional< veto >;

		// Indexed by kind. A dense enum means a plain array, and per-kind lists mean
		// no subscriber pays for event types it does not want.
		std::vector< entry > subscribers_[ KIND_COUNT ];

		std::deque< event > pending_;
		subscription_id next_id_ = 1;
		bool dispatching_ = false;
		std::uint64_t handler_failures_ = 0;
		std::uint64_t overflow_drops_ = 0;
	};

}
