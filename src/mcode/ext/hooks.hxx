#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/lua_host.hxx"

namespace mcode::ext {

	// One `mcode.on` subscription.
	struct hook_subscription {
		events::bus::subscription_id id = 0;

		// The event name the extension asked for, verbatim. A name that is not a
		// session event kind is a custom event (docs/18 `emit`).
		std::string name;

		// The handler, by registry reference.
		int function_reference = 0;

		std::string owner;

		// The VM the handler lives in. Held so a bus callback -- which the bus
		// invokes with no context of ours -- can find the VM to call into.
		lua_host* host = nullptr;

		// Only the three kinds docs/20 marks vetoable accept a veto return.
		bool vetoable = false;

		// Set for a session event; unset for a custom event, which never reaches
		// the bus.
		bool on_bus = false;
	};

	// Routes bus events and custom extension events into Luau handlers.
	//
	// Custom events deliberately do NOT enter the bus. docs/20 chooses a closed
	// tagged union precisely so the session log's schema stays explicit, and
	// `kind` is the log's primary key -- so an extension-invented event name must
	// not become a kind. It dispatches here instead, and never reaches the log.
	class hook_registry {
	public:
		// The bus is bound here rather than passed per call, so the lifetime
		// requirement is in the type: the registry unsubscribes in its destructor,
		// and a bus that dies first leaves the registry writing into freed memory.
		// A caller declares the bus first and the registry second.
		explicit hook_registry( events::bus& bus );

		hook_registry( hook_registry&& other ) noexcept = delete;
		auto operator=( hook_registry&& other ) noexcept -> hook_registry& = delete;

		hook_registry( const hook_registry& ) = delete;
		auto operator=( const hook_registry& ) -> hook_registry& = delete;

		~hook_registry( );

		// Subscribes a handler. `name` is a session event name or a custom one; a
		// name that is neither is refused, because a typo would otherwise register
		// a hook that can never fire.
		auto subscribe( lua_host& host, std::string_view name, int function_reference,
			std::string owner ) -> result< std::uint64_t >;

		auto unsubscribe( std::uint64_t identifier ) -> bool;

		// Fires a custom event. Returns the veto reason when a handler blocked it.
		//
		// Custom events are not vetoable in v1: a veto is a gate on a host action,
		// and a custom event is one extension talking to another, where a refusal
		// is the sender's business rather than the harness's.
		auto emit( lua_host& host, std::string_view name, std::string_view payload_json )
			-> status;

		// Detaches every subscription an extension owns, releasing its references.
		auto detach_owner( lua_host& host, std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return subscriptions_.size( ); }

		[[nodiscard]] auto handlers_for( std::string_view name ) const -> std::size_t;

		// Consecutive failures for the extension's worst handler, which is what the
		// quarantine threshold in docs/18 counts.
		[[nodiscard]] auto failures( std::string_view owner ) const -> std::uint32_t;

		// The subscription with this identifier, or null.
		[[nodiscard]] auto find( std::uint64_t identifier ) const -> const hook_subscription*;

		// Handler failures observed across all extensions.
		[[nodiscard]] auto total_failures( ) const noexcept -> std::uint64_t {
			return total_failures_;
		}

	private:
		// Calls one handler with an event table built from the event fields. Returns
		// the veto when the handler returned one, and reports a failure by counting
		// it rather than throwing: docs/20 makes dispatch noexcept at the boundary.
		auto call_handler( const hook_subscription& subscription, events::kind type,
			std::uint64_t sequence, std::int64_t timestamp_ms,
			std::string_view payload_json ) -> std::optional< events::veto >;

		auto record_failure( std::uint64_t identifier ) -> void;
		auto record_success( std::uint64_t identifier ) -> void;

		events::bus& bus_;

		std::vector< hook_subscription > subscriptions_;
		std::map< std::uint64_t, std::uint32_t > consecutive_failures_;
		std::uint64_t next_identifier_ = 1;
		std::uint64_t total_failures_ = 0;

		// Bus subscriptions this registry created. Held so `~hook_registry` removes
		// them: the bus's closures look a subscription up by id, and one that
		// outlived its registry would call through a dangling pointer.
		std::vector< events::bus::subscription_id > bus_links_;
	};

	// The session event names docs/18 documents, and the kind each maps to.
	[[nodiscard]] auto session_event_kind( std::string_view name ) -> std::optional< events::kind >;

	// True when a name's first segment is one the session events already own
	// (`tool`, `session`, `turn`, …) while the full name is not a session event.
	//
	// `tool.precal` is a typo for `tool.pre_call`, and a hook that can never fire is
	// worse than a load error: the author would believe their guard was active. The
	// reserved set is derived from the session event table, so it cannot drift from
	// the names it is guarding.
	[[nodiscard]] auto has_reserved_prefix( std::string_view name ) -> bool;

}
