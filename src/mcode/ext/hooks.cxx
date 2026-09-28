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

		// Five consecutive failures detach every handler an extension owns. A
		// cumulative count would quarantine an extension that fails rarely but
		// forever, which is why the streak is cleared by any clean call.
		inline constexpr auto QUARANTINE_THRESHOLD = std::uint32_t{ 5 };


		// The documented session event names. Anything not in this table is
		// treated as a custom event, so a typo like `tool.precal` registers a hook
		// that silently never fires -- which is why `session_event_kind` exists and
		// the table is the single source of both answers.
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


		// The handler shape: one plain-data table with the event name, the
		// sequence, and the payload. `payload` is the bus's payload_json embedded
		// verbatim -- parsing and re-serializing it would be a second chance to
		// corrupt the event the handler sees.
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

			// Only an object can be spliced in as `payload`. A payload that is not
			// an object -- a bare string or an array -- would produce invalid JSON
			// here, so it is wrapped rather than trusted.
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

		// The closure reference is owned by the subscription, so dropping the
		// subscription must drop it. Leaking it would keep the extension's function
		// alive in a VM that is about to be destroyed, and a VM already destroyed
		// cannot be unrefed at all -- so this only runs while the host is alive,
		// which is why the caller passes it.
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
		// The worst of the extension's handlers, not the sum and not the last.
		// Counting per extension would let one healthy handler clear a flapping
		// sibling's streak forever, which defeats the quarantine threshold.
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

		// The threshold is not advisory: counting without enforcing is a counter,
		// not a quarantine. Every handler for the extension goes, so a flapping
		// sibling cannot keep it alive, and the extension is recorded so a later
		// subscribe is refused rather than silently detached again.
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

		// Reported, because an extension that quietly stopped working is the
		// failure this threshold exists to surface.
		std::fprintf( stderr, "[%s] quarantined after %u consecutive handler failures\n",
			owner.c_str( ), QUARANTINE_THRESHOLD );

		detach_owner( *host, owner );
	}

	auto hook_registry::record_success( const std::uint64_t identifier ) -> void {
		// CONSECUTIVE failures are counted: one clean call clears the counter, so
		// an extension that fails once every hundred events is never quarantined.
		consecutive_failures_[ identifier ] = 0;
	}

	auto hook_registry::subscribe( lua_host& host, const std::string_view name,
		const int function_reference, std::string owner ) -> result< std::uint64_t > {
		// A quarantined extension stays quarantined: re-registering would restore
		// the handlers the threshold just removed, which is the whole thing the
		// quarantine is for.
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

		// The bus owns session kinds; a custom name never reaches it, because
		// the closed union is the log's schema and an extension-invented name
		// must not become a kind.
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

			// The bus closure looks the subscription up by id and finds nothing, so
			// removing it here is enough -- but the bus still holds a dead entry and
			// the VM still holds the closure, so both are released.
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
		// A quarantined extension does not run. Its handlers are already detached,
		// so this is the path a custom-event dispatch takes for one, and it must
		// not resurrect the VM call.
		if ( quarantined_.contains( subscription.owner ) ) {
			return std::nullopt;
		}

		auto* state = subscription.host != nullptr ? subscription.host->raw( ) : nullptr;

		if ( state == nullptr ) {
			record_failure( subscription.id );

			return std::nullopt;
		}

		// The 50 ms budget is a property of the DISPATCH, not of the entry point.
		// Without this an extension could spin forever inside a hook or a tool call,
		// because those paths never went through run/eval/call_global.
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

		// The handler receives a decoded table, not a JSON string: the API promises
		// `ev.args.cmd`, and handing the extension text to parse would make every
		// author write the same decoder.
		if ( auto pushed = push_json( state, table ); !pushed ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		// A handler that throws is contained and counted, never allowed to abort
		// dispatch (dispatch is noexcept at the boundary).
		if ( lua_pcall( state, 1, 1, 0 ) != 0 ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		auto outcome = std::optional< events::veto >{ };

		// Bare `false` is sugar for a veto. It has to be handled here rather than
		// left to the table check below, because a handler that returns false and is
		// silently ignored leaves the author believing a guard is active.
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

		// A snapshot: a handler may subscribe or unsubscribe, and dispatching over a
		// mutating list is the reentrancy bug the bus guards against.
		auto targets = std::vector< hook_subscription >{ };

		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.name == name ) {
				targets.push_back( subscription );
			}
		}

		const auto timestamp = support::epoch_milliseconds( );

		for ( const auto& subscription : targets ) {
			// The identity comes from the subscription. A custom event is not a
			// session event, so there is no `events::kind` to name it -- stamping it
			// with a made-up one would make `ev.event` read "session.start" inside
			// every custom handler.
			// Custom events are not vetoable: a veto is a gate on a host action, and
			// this is one extension talking to another, so a refusal is the sender's
			// business rather than the harness's. The result is therefore discarded
			// rather than propagated.
			call_handler( subscription,
				delivered_event{ .name = subscription.name, .sequence = ++custom_sequence_,
					.timestamp_ms = timestamp, .payload_json = payload_json } );
		}

		return { };
	}

}
