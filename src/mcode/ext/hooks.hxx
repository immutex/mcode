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

	struct hook_subscription {
		std::uint64_t id = 0;

		// zero means a custom event, which never leaves this registry.
		events::bus::subscription_id bus_id = 0;

		std::string name;
		int function_reference = 0;
		std::string owner;
		lua_host* host = nullptr;

		// only the three kinds the bus marks vetoable accept a veto return.
		bool vetoable = false;
	};

	// custom events never enter the bus: its kind is the session log's primary key.
	class hook_registry {
	public:
		// requires the bus to outlive the registry: the destructor unsubscribes through it.
		explicit hook_registry( events::bus& bus );

		hook_registry( hook_registry&& other ) noexcept = delete;
		auto operator=( hook_registry&& other ) noexcept -> hook_registry& = delete;

		hook_registry( const hook_registry& ) = delete;
		auto operator=( const hook_registry& ) -> hook_registry& = delete;

		~hook_registry( );

		auto subscribe( lua_host& host, std::string_view name, int function_reference,
			std::string owner ) -> result< std::uint64_t >;

		auto unsubscribe( std::uint64_t identifier ) -> bool;

		// custom events are not vetoable: a veto gates a host action.
		auto emit( std::string_view name, std::string_view payload_json )
			-> status;

		auto detach_owner( lua_host& host, std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return subscriptions_.size( ); }

		[[nodiscard]] auto handlers_for( std::string_view name ) const -> std::size_t;

		// the worst handler's streak, which is what the quarantine threshold counts.
		[[nodiscard]] auto failures( std::string_view owner ) const -> std::uint32_t;

		[[nodiscard]] auto find( std::uint64_t identifier ) const -> const hook_subscription*;

		[[nodiscard]] auto total_failures( ) const noexcept -> std::uint64_t {
			return total_failures_;
		}

		[[nodiscard]] auto quarantined( std::string_view owner ) const -> bool {
			return quarantined_.contains( owner );
		}

	private:
		struct delivered_event {
			std::string_view name;
			std::uint64_t sequence = 0;
			std::int64_t timestamp_ms = 0;
			std::string_view payload_json;
		};

		auto call_handler( const hook_subscription& subscription, const delivered_event& delivered )
			-> std::optional< events::veto >;

		auto detach( lua_host& host, std::vector< hook_subscription >::iterator entry ) -> void;

		auto record_failure( std::uint64_t identifier ) -> void;
		auto record_success( std::uint64_t identifier ) -> void;

		events::bus& bus_;

		std::set< std::string, std::less<> > quarantined_;

		std::vector< hook_subscription > subscriptions_;

		// custom events have no bus sequence to borrow, so they are numbered here.
		std::uint64_t custom_sequence_ = 0;
		std::map< std::uint64_t, std::uint32_t > consecutive_failures_;
		std::uint64_t next_identifier_ = 1;
		std::uint64_t total_failures_ = 0;
	};

	[[nodiscard]] auto session_event_kind( std::string_view name ) -> std::optional< events::kind >;

	// true when the name has a session-event prefix but is not an event, e.g. a `tool.precal` typo.
	[[nodiscard]] auto has_reserved_prefix( std::string_view name ) -> bool;

}
