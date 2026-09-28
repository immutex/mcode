#include "mcode/ext/hooks.hxx"

#include <optional>
#include <string>
#include <utility>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/lua_json.hxx"
#include "mcode/support/json.hxx"

namespace mcode::ext {

	namespace {

		// The session event names docs/18 documents. Anything not in this table is
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

		auto append_escaped( std::string& out, const std::string_view text ) -> void {
			for ( const auto character : text ) {
				switch ( character ) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;

					default:
						out += character;
				}
			}
		}

		// docs/18's handler shape: one plain-data table with the event name, the
		// sequence, and the payload. `payload` is the bus's payload_json embedded
		// verbatim -- parsing and re-serializing it would be a second chance to
		// corrupt the event the handler sees.
		auto event_table( const std::string_view name, const std::uint64_t sequence,
			const std::int64_t timestamp_ms, const std::string_view payload_json )
			-> std::string {
			auto out = std::string{ };
			out.reserve( 96 + name.size( ) + payload_json.size( ) );

			out += "{\"event\":\"";
			append_escaped( out, name );
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
		for ( const auto identifier : bus_links_ ) {
			bus_.unsubscribe( identifier );
		}
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
		++consecutive_failures_[ identifier ];
	}

	auto hook_registry::record_success( const std::uint64_t identifier ) -> void {
		// docs/18 counts CONSECUTIVE failures: one clean call clears the counter, so
		// an extension that fails once every hundred events is never quarantined.
		consecutive_failures_[ identifier ] = 0;
	}

	auto hook_registry::subscribe( lua_host& host, const std::string_view name,
		const int function_reference, std::string owner ) -> result< std::uint64_t > {
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
		// docs/20's closed union is the log's schema and an extension-invented name
		// must not become a kind.
		if ( const auto kind = session_event_kind( name ) ) {
			subscription.on_bus = true;
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

						return call_handler( *found, value.type, value.sequence, value.timestamp_ms,
							value.payload_json );
					} );

				bus_links_.push_back( link );
			} else {
				const auto link = bus_.subscribe( *kind,
					[ this, identifier ]( const events::event& value ) {
						const auto* found = find( identifier );

						if ( found == nullptr ) {
							return;
						}

						call_handler( *found, value.type, value.sequence, value.timestamp_ms,
							value.payload_json );
					} );

				bus_links_.push_back( link );
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

			// The closure the bus holds looks the subscription up by id and finds
			// nothing, so removing it here is enough: no bus entry can outlive the
			// subscription it dispatches to.
			subscriptions_.erase( iterator );

			return true;
		}

		return false;
	}

	auto hook_registry::detach_owner( lua_host& host, const std::string_view owner ) -> std::size_t {
		const auto removed = std::erase_if( subscriptions_,
			[owner]( const hook_subscription& subscription ) {
				return subscription.owner == owner;
			} );

		for ( const auto& subscription : subscriptions_ ) {
			(void)subscription;
		}

		consecutive_failures_.clear( );

		(void)host;

		return removed;
	}

	auto hook_registry::call_handler( const hook_subscription& subscription, const events::kind type,
		const std::uint64_t sequence, const std::int64_t timestamp_ms,
		const std::string_view payload_json ) -> std::optional< events::veto > {
		auto* state = subscription.host != nullptr ? subscription.host->raw( ) : nullptr;

		if ( state == nullptr ) {
			record_failure( subscription.id );

			return std::nullopt;
		}

		const auto depth = lua_gettop( state );

		lua_getref( state, subscription.function_reference );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		const auto table = event_table( to_string( type ), sequence, timestamp_ms, payload_json );

		// The handler receives a decoded table, not a JSON string: docs/18 promises
		// `ev.args.cmd`, and handing the extension text to parse would make every
		// author write the same decoder.
		if ( auto pushed = push_json( state, table ); !pushed ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		// A handler that throws is contained and counted, never allowed to abort
		// dispatch (docs/20: dispatch is noexcept at the boundary).
		if ( lua_pcall( state, 1, 1, 0 ) != 0 ) {
			lua_settop( state, depth );
			record_failure( subscription.id );

			return std::nullopt;
		}

		auto outcome = std::optional< events::veto >{ };

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

	auto hook_registry::emit( lua_host& host, const std::string_view name,
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
		// mutating list is the reentrancy bug docs/20 guards against.
		auto targets = std::vector< hook_subscription >{ };

		for ( const auto& subscription : subscriptions_ ) {
			if ( subscription.name == name ) {
				targets.push_back( subscription );
			}
		}

		for ( const auto& subscription : targets ) {
			(void)call_handler( subscription, events::kind::session_start, 0, 0, payload_json );
		}

		(void)host;

		return { };
	}

}
