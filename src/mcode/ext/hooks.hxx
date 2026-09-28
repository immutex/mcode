#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/lua_host.hxx"

namespace mcode::ext {

	// One `mcode.on` subscription.
	struct hook_subscription {
		// This registry's identifier, which `mcode.off` takes.
		std::uint64_t id = 0;

		// The bus's identifier, when the subscription reached the bus. Zero means
		// a custom event, which never leaves this registry.
		events::bus::subscription_id bus_id = 0;

		// The event name the extension asked for, verbatim. A name that is not a
		// session event kind is a custom event.
		std::string name;

		// The handler, by registry reference. The registry releases it, so a
		// subscription is the only owner of the reference.
		int function_reference = 0;

		std::string owner;

		// The VM the handler lives in. Held so a bus callback -- which the bus
		// invokes with no context of ours -- can find the VM to call into.
		lua_host* host = nullptr;

		// Only the three kinds the bus marks vetoable accept a veto return.
		bool vetoable = false;
	};

	// Routes bus events and custom extension events into Luau handlers.
	//
	// Custom events deliberately do NOT enter the bus. The bus's kind is a closed
	// union because it is the session log's primary key, so an extension-invented
	// event name must not become a kind. It dispatches here instead and never
	// reaches the log.
	class hook_registry {
	public:
		// The bus is bound here rather than passed per call.
		//
		// REQUIRES the bus to outlive the registry: the destructor unsubscribes,
		// and a bus destroyed first leaves the registry writing into freed memory.
		// A reference member does not enforce that -- it only makes the dependency
		// visible -- so the caller must declare the bus first and the registry
		// second. Both owners in this tree do.
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

		// Fires a custom event.
		//
		// Custom events are not vetoable: a veto is a gate on a host action, and a
		// custom event is one extension talking to another, where a refusal is the
		// sender's business rather than the harness's.
		auto emit( std::string_view name, std::string_view payload_json )
			-> status;

		// Detaches every subscription an extension owns: unsubscribes it from the
		// bus, releases its closure reference, and drops its failure count. This is
		// what makes a discarded VM safe -- a hook that outlived its VM would be
		// dispatched into freed memory.
		auto detach_owner( lua_host& host, std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return subscriptions_.size( ); }

		[[nodiscard]] auto handlers_for( std::string_view name ) const -> std::size_t;

		// Consecutive failures for the extension's worst handler, which is what the
		// quarantine threshold counts.
		[[nodiscard]] auto failures( std::string_view owner ) const -> std::uint32_t;

		// The subscription with this identifier, or null.
		[[nodiscard]] auto find( std::uint64_t identifier ) const -> const hook_subscription*;

		// Handler failures observed across all extensions.
		[[nodiscard]] auto total_failures( ) const noexcept -> std::uint64_t {
			return total_failures_;
		}

		// Whether the extension has tripped the consecutive-failure quarantine and
		// had every handler detached.
		[[nodiscard]] auto quarantined( std::string_view owner ) const -> bool {
			return quarantined_.contains( owner );
		}

	private:
		// Calls one handler with an event table built from the event fields. Returns
		// the veto when the handler returned one, and reports a failure by counting
		// it rather than throwing: dispatch is noexcept at the bus boundary.
		// A session event passes the name of its kind; a custom event passes its
		// own, which has no kind to be derived from. The four fields travel together
		// because they are what the handler's event table is built from.
		struct delivered_event {
			std::string_view name;
			std::uint64_t sequence = 0;
			std::int64_t timestamp_ms = 0;
			std::string_view payload_json;
		};

		auto call_handler( const hook_subscription& subscription, const delivered_event& delivered )
			-> std::optional< events::veto >;

		// Drops one subscription: bus link, closure reference, failure count.
		auto detach( lua_host& host, std::vector< hook_subscription >::iterator entry ) -> void;

		auto record_failure( std::uint64_t identifier ) -> void;
		auto record_success( std::uint64_t identifier ) -> void;

		events::bus& bus_;

		// Extensions whose handlers were detached for failing the threshold.
		std::set< std::string, std::less<> > quarantined_;

		std::vector< hook_subscription > subscriptions_;

		// Custom events have no bus sequence to borrow, so they are numbered here.
		std::uint64_t custom_sequence_ = 0;
		std::map< std::uint64_t, std::uint32_t > consecutive_failures_;
		std::uint64_t next_identifier_ = 1;
		std::uint64_t total_failures_ = 0;
	};

	// The session event names the API documents, and the kind each maps to.
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
