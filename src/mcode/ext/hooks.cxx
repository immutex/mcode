#include "mcode/ext/hooks.hxx"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/lua_json.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

namespace mcode::ext {

	namespace {

		// consecutive, not cumulative: one clean call clears the streak.
		inline constexpr auto QUARANTINE_THRESHOLD = std::uint32_t{ 5 };


		const struct {
			const char* name;
			events::kind kind;
		} SESSION_EVENTS[] = {
			{ "session.start", events::kind::session_start },
			{ "session.end", events::kind::session_end },
			{ "turn.start", events::kind::turn_start },
			{ "turn.end", events::kind::turn_end },
			{ "step.start", events::kind::step_start },
			{ "step.end", events::kind::step_end },
			{ "assistant.delta", events::kind::assistant_delta },
			{ "tool.pre_call", events::kind::tool_pre_call },
			{ "tool.call", events::kind::tool_call },
			{ "tool.result", events::kind::tool_result },
			{ "spawn.pre", events::kind::spawn_pre },
			{ "prompt.pre", events::kind::prompt_pre },
			{ "extension.loaded", events::kind::extension_loaded },
			{ "extension.error", events::kind::extension_error },
			{ "context.compaction", events::kind::compaction },
			{ "session.error", events::kind::error },
		};


		// the payload is the bus's payload_json spliced in verbatim, not re-serialized.
		auto event_table( const std::string_view name, const std::uint64_t sequence,
			const std::int64_t timestamp_ms, const std::string_view payload_json )
			-> std::string {
			auto out = std::string{ };
			out.reserve( 96 + name.size( ) + payload_json.size( ) );

			out += "{\"event\":\"";
			json::append_escaped( out, name );
			out += "\",\"seq\":";
			out += std::to_string( sequence );
			out += ",\"ts\":";
			out += std::to_string( timestamp_ms );

			const auto payload = payload_json.empty( ) ? std::string_view{ "{}" } : payload_json;

			if ( payload.front( ) == '{' ) {
				out += ",\"payload\":";
				out += payload;
			} else {
				out += ",\"payload\":{\"value\":";
				out += payload;
				out += '}';
			}

			out += '}';

			return out;
		}

	}

	auto session_event_kind( const std::string_view name ) -> std::optional< events::kind > {
		for ( const auto& entry : SESSION_EVENTS ) {
			if ( name == entry.name ) {
				return entry.kind;
			}
		}

		return std::nullopt;
	}

	auto has_reserved_prefix( const std::string_view name ) -> bool {
		if ( session_event_kind( name ) ) {
			return false;
		}

		const auto separator = name.find( '.' );

		if ( separator == std::string_view::npos ) {
			return false;
		}

		const auto prefix = name.substr( 0, separator );

		for ( const auto& entry : SESSION_EVENTS ) {
			const auto owned = std::string_view{ entry.name };
			const auto owned_separator = owned.find( '.' );

			if ( owned_separator != std::string_view::npos &&
				owned.substr( 0, owned_separator ) == prefix ) {
				return true;
			}
		}

		return false;
	}

	hook_registry::hook_registry( events::bus& bus )
		: bus_( bus ) { }

	hook_registry::~hook_registry( ) {
		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.bus_id != 0 ) {
				bus_.unsubscribe( subscription.bus_id );
			}
		}
	}

	auto hook_registry::detach( lua_host& host, std::vector< hook_subscription >::iterator entry )
		-> void {
		if ( entry->bus_id != 0 ) {
			bus_.unsubscribe( entry->bus_id );
		}

		// must unref while the VM is alive; a destroyed VM cannot be unrefed at all.
		if ( host.raw( ) != nullptr && entry->function_reference != 0 ) {
			lua_unref( host.raw( ), entry->function_reference );
		}

		consecutive_failures_.erase( entry->id );
	}

	auto hook_registry::find( const std::uint64_t identifier ) const -> const hook_subscription* {
		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.id == identifier ) {
				return &subscription;
			}
		}

		return nullptr;
	}

	auto hook_registry::handlers_for( const std::string_view name ) const -> std::size_t {
		auto count = std::size_t{ 0 };

		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.name == name ) {
				++count;
			}
		}

		return count;
	}

	auto hook_registry::failures( const std::string_view owner ) const -> std::uint32_t {
		// worst, not sum: a healthy sibling must not clear a flapping one's streak.
		auto worst = std::uint32_t{ 0 };

		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.owner != owner ) {
				continue;
			}

			const auto found = consecutive_failures_.find( subscription.id );

			if ( found != consecutive_failures_.end( ) && found->second > worst ) {
				worst = found->second;
			}
		}

		return worst;
	}

	auto hook_registry::record_failure( const std::uint64_t identifier ) -> void {
		++total_failures_;

		const auto& count = ++consecutive_failures_[ identifier ];

		if ( count < QUARANTINE_THRESHOLD ) {
			return;
		}

		const auto* subscription = find( identifier );

		if ( subscription == nullptr ) {
			return;
		}

		const auto owner = subscription->owner;
		auto* host = subscription->host;

		if ( host == nullptr ) {
			return;
		}

		quarantined_.insert( owner );

		std::fprintf( stderr, "[%s] quarantined after %u consecutive handler failures\n",
			owner.c_str( ), QUARANTINE_THRESHOLD );

		detach_owner( *host, owner );
	}

	auto hook_registry::record_success( const std::uint64_t identifier ) -> void {
		consecutive_failures_[ identifier ] = 0;
	}

	auto hook_registry::subscribe( lua_host& host, const std::string_view name,
		const int function_reference, std::string owner ) -> result< std::uint64_t > {
		if ( quarantined_.contains( owner ) ) {
			return std::unexpected( fail( errc::config,
				"extension '" + owner + "' is quarantined after repeated handler failures" ) );
		}

		if ( name.empty( ) ) {
			return std::unexpected( fail( errc::config, "mcode.on: the event name is required" ) );
		}

		auto subscription = hook_subscription{ };
		subscription.id = next_identifier_++;
		subscription.name = std::string{ name };
		subscription.function_reference = function_reference;
		subscription.owner = std::move( owner );
		subscription.host = &host;

		if ( const auto kind = session_event_kind( name ) ) {
			subscription.vetoable = events::is_vetoable( *kind );

			const auto identifier = subscription.id;

			if ( subscription.vetoable ) {
				const auto link = bus_.subscribe_veto( *kind,
					[ this, identifier ]( const events::event& value )
						-> std::optional< events::veto > {
						const auto* found = find( identifier );

						if ( found == nullptr ) {
							return std::nullopt;
						}

						return call_handler( *found,
							delivered_event{ .name = to_string( value.type ),
								.sequence = value.sequence, .timestamp_ms = value.timestamp_ms,
								.payload_json = value.payload_json } );
					} );

				subscription.bus_id = link;
			} else {
				const auto link = bus_.subscribe( *kind,
					[ this, identifier ]( const events::event& value ) {
						const auto* found = find( identifier );

						if ( found == nullptr ) {
							return;
						}

						call_handler( *found,
							delivered_event{ .name = to_string( value.type ),
								.sequence = value.sequence, .timestamp_ms = value.timestamp_ms,
								.payload_json = value.payload_json } );
					} );

				subscription.bus_id = link;
			}
		}

		const auto identifier = subscription.id;

		subscriptions_.push_back( std::move( subscription ) );

		return identifier;
	}

	auto hook_registry::unsubscribe( const std::uint64_t identifier ) -> bool {
		for ( auto iterator = subscriptions_.begin( ); iterator != subscriptions_.end( );
			++iterator ) {
			if ( iterator->id != identifier ) {
				continue;
			}

			auto* host = iterator->host;

			if ( host != nullptr ) {
				detach( *host, iterator );
			} else {
				consecutive_failures_.erase( iterator->id );
			}

			subscriptions_.erase( iterator );

			return true;
		}

		return false;
	}

	auto hook_registry::detach_owner( lua_host& host, const std::string_view owner ) -> std::size_t {
		auto removed = std::size_t{ 0 };

		for ( auto iterator = subscriptions_.begin( ); iterator != subscriptions_.end( ); ) {
			if ( iterator->owner != owner ) {
				++iterator;

				continue;
			}

			detach( host, iterator );
			iterator = subscriptions_.erase( iterator );
			++removed;
		}

		return removed;
	}

	auto hook_registry::call_handler( const hook_subscription& subscription,
		const delivered_event& delivered ) -> std::optional< events::veto > {
		if ( quarantined_.contains( subscription.owner ) ) {
			return std::nullopt;
		}

		auto* state = subscription.host != nullptr ? subscription.host->raw( ) : nullptr;

		if ( state == nullptr ) {
			record_failure( subscription.id );

			return std::nullopt;
		}

		// budgets the dispatch: hooks and tool calls never go through run/eval.
		auto budget = lua_host::budget_scope{ subscription.host };

		const auto depth = lua_gettop( state );

		lua_getref( state, subscription.function_reference );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		const auto table = event_table( delivered.name, delivered.sequence,
			delivered.timestamp_ms, delivered.payload_json );

		if ( auto pushed = push_json( state, table ); !pushed ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		if ( lua_pcall( state, 1, 1, 0 ) != 0 ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		auto outcome = std::optional< events::veto >{ };

		if ( subscription.vetoable && lua_type( state, -1 ) == LUA_TBOOLEAN &&
			lua_toboolean( state, -1 ) == 0 ) {
			auto reason = events::veto{ };
			reason.reason = "vetoed by " + subscription.owner;
			reason.source = subscription.owner;

			outcome = reason;
		}

		if ( subscription.vetoable && lua_type( state, -1 ) == LUA_TTABLE ) {
			lua_getfield( state, -1, "veto" );

			if ( lua_type( state, -1 ) == LUA_TSTRING ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, -1, &length );

				if ( text != nullptr ) {
					auto reason = events::veto{ };
					reason.reason.assign( text, length );
					reason.source = subscription.owner;

					outcome = reason;
				}
			}

			lua_pop( state, 1 );
		}

		lua_settop( state, depth );
		record_success( subscription.id );

		return outcome;
	}

	auto hook_registry::emit( const std::string_view name,
		const std::string_view payload_json ) -> status {
		if ( name.empty( ) ) {
			return std::unexpected( fail( errc::config, "mcode.emit: the event name is required" ) );
		}

		if ( session_event_kind( name ) ) {
			return std::unexpected( fail( errc::config,
				"mcode.emit: '" + std::string{ name } + "' is a session event; use mcode.on to "
				"observe it, and emit only custom events" ) );
		}

		// a snapshot: a handler may subscribe or unsubscribe during dispatch.
		auto targets = std::vector< hook_subscription >{ };

		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.name == name ) {
				targets.push_back( subscription );
			}
		}

		const auto timestamp = support::epoch_milliseconds( );

		for ( const auto& subscription : targets ) {
			call_handler( subscription,
				delivered_event{ .name = subscription.name, .sequence = ++custom_sequence_,
					.timestamp_ms = timestamp, .payload_json = payload_json } );
		}

		return { };
	}

}
