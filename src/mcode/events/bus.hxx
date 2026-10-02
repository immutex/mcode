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

	// The numeric values are the log's primary key: append a new kind, never renumber one.
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
		assistant_thinking = 16,
	};

	inline constexpr std::size_t KIND_COUNT =
		static_cast< std::size_t >( kind::assistant_thinking ) + 1;

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
			case kind::assistant_thinking: return "assistant.thinking";
		}

		return "unknown";
	}

	// A handler publishing on every event it receives would otherwise spin forever.
	inline constexpr std::size_t MAX_EVENTS_PER_PUBLISH = 10'000;

	// Everything else is notification-only, so a handler cannot block a turn by returning.
	[[nodiscard]] constexpr auto is_vetoable( const kind value ) noexcept -> bool {
		return value == kind::tool_pre_call || value == kind::spawn_pre ||
			value == kind::prompt_pre;
	}

	struct event {
		kind type = kind::session_start;

		std::uint64_t sequence = 0;
		std::int64_t timestamp_ms = 0;

		// Owned, not borrowed: producers run on worker threads and the loop consumes later.
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

		using veto_handler = std::function< std::optional< veto >( const event& ) >;

		bus( ) = default;

		auto subscribe( kind type, handler function ) -> subscription_id;
		auto subscribe_veto( kind type, veto_handler function ) -> subscription_id;

		// Swap-remove plus a tombstone, so unsubscribing mid-dispatch cannot invalidate iteration.
		auto unsubscribe( subscription_id id ) -> void;

		// Returns the veto when a vetoable kind was blocked; never for other kinds.
		auto publish( event value ) -> std::optional< veto >;

		// Drained after the current dispatch completes, so nesting stays flat.
		[[nodiscard]] auto pending_count( ) const noexcept -> std::size_t {
			return pending_.size( );
		}

		[[nodiscard]] auto overflow_drops( ) const noexcept -> std::uint64_t {
			return overflow_drops_;
		}

		[[nodiscard]] auto dispatching( ) const noexcept -> bool { return dispatching_; }

		[[nodiscard]] auto subscriber_count( kind type ) const noexcept -> std::size_t;

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

		std::vector< entry > subscribers_[ KIND_COUNT ];

		std::deque< event > pending_;
		subscription_id next_id_ = 1;
		bool dispatching_ = false;
		std::uint64_t handler_failures_ = 0;
		std::uint64_t overflow_drops_ = 0;
	};

}
