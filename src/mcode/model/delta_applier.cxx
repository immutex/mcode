#include "mcode/model/delta_applier.hxx"

#include <optional>

#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

		// Extracts a field only if the pointer is mapped and present. A pointer
		// that is not mapped is not an error -- most providers omit most fields on
		// most events -- so absence and "not mapped" both yield nothing.
		auto optional_string( const json::document& doc, const std::string& pointer )
			-> std::optional< std::string > {
			if ( pointer.empty( ) ) {
				return std::nullopt;
			}

			if ( auto value = doc.pointer_string( pointer ) ) {
				return *value;
			}

			return std::nullopt;
		}

		auto optional_int( const json::document& doc, const std::string& pointer )
			-> std::optional< std::int64_t > {
			if ( pointer.empty( ) ) {
				return std::nullopt;
			}

			if ( auto value = doc.pointer_int( pointer ) ) {
				return *value;
			}

			return std::nullopt;
		}

	}

	delta_applier::delta_applier( const provider_descriptor& descriptor )
		: descriptor_( descriptor ) { }

	delta_applier::delta_applier( provider_descriptor&& descriptor )
		: descriptor_( std::move( descriptor ) ) { }

	auto delta_applier::pending_for( const int index ) -> pending_call& {
		for ( auto& call : pending_ ) {
			if ( call.index == index ) {
				return call;
			}
		}

		pending_.push_back( pending_call{ .index = index } );

		return pending_.back( );
	}

	auto delta_applier::feed( const std::string_view event_name, const std::string_view data )
		-> result< std::vector< chat_event > > {
		auto produced = std::vector< chat_event >{ };

		for ( const auto& terminal : descriptor_.stream.terminal_events ) {
			if ( event_name == terminal ) {
				terminal_seen_ = true;

				return produced;
			}
		}

		// The [DONE] sentinel is a bare payload, not JSON. Every OpenAI-compatible
		// endpoint sends it, so it is handled structurally rather than by asking
		// descriptors to declare it.
		if ( data == "[DONE]" ) {
			terminal_seen_ = true;

			return produced;
		}

		if ( data.empty( ) ) {
			return produced;
		}

		auto parsed = json::document::parse( data );

		if ( !parsed ) {
			return std::unexpected( fail( errc::protocol,
				"stream event is not JSON: " + parsed.error( ).msg ) );
		}

		if ( auto text = optional_string( *parsed, descriptor_.stream.text_delta ) ) {
			if ( !text->empty( ) ) {
				auto event = chat_event{ };
				event.type = chat_event::kind::text_delta;
				event.text = *text;

				produced.push_back( std::move( event ) );
			}
		}

		if ( auto thinking = optional_string( *parsed, descriptor_.stream.thinking_delta ) ) {
			if ( !thinking->empty( ) ) {
				auto event = chat_event{ };
				event.type = chat_event::kind::thinking_delta;
				event.text = *thinking;

				produced.push_back( std::move( event ) );
			}
		}

		// Tool-call fragments. The index defaults to 0 for providers that only
		// ever send one call and omit the field.
		const auto index = optional_int( *parsed, descriptor_.stream.tool_call_index )
			.value_or( 0 );

		auto id_fragment = optional_string( *parsed, descriptor_.stream.tool_call_id );
		auto name_fragment = optional_string( *parsed, descriptor_.stream.tool_call_name );
		auto args_fragment = optional_string( *parsed, descriptor_.stream.tool_call_args );

		if ( id_fragment || name_fragment || args_fragment ) {
			auto& call = pending_for( static_cast< int >( index ) );

			// Fragments concatenate: a provider splits a call's arguments across
			// arbitrary events, so the id and name are also treated as fragments
			// rather than overwritten.
			if ( id_fragment ) {
				call.id += *id_fragment;
			}

			if ( name_fragment ) {
				call.name += *name_fragment;
			}

			if ( args_fragment ) {
				call.args_fragments += *args_fragment;
			}

			// Emitted on any fragment, not only once a name is known. The index is
			// what correlates fragments, and some gateways send arguments before the
			// name -- waiting would drop those fragments from the stream while still
			// accumulating them. Whether a call is *usable* is decided in finish(),
			// which is the only point where the fragments are complete.
			auto event = chat_event{ };
			event.type = chat_event::kind::tool_call_delta;
			event.index = call.index;
			event.tool_call_id = call.id;
			event.tool_name = call.name;
			event.args_fragment = args_fragment.value_or( std::string{ } );

			produced.push_back( std::move( event ) );
		}

		if ( auto reason = optional_string( *parsed, descriptor_.stream.finish_reason ) ) {
			stop_reason_ = *reason;
		}

		// Usage. Reported as deltas by some providers and cumulatively by others;
		// usage::add takes the max, which is correct for both.
		auto input = optional_int( *parsed, descriptor_.stream.usage_input );
		auto output = optional_int( *parsed, descriptor_.stream.usage_output );
		auto cached = optional_int( *parsed, descriptor_.stream.usage_cached_read );
		auto written = optional_int( *parsed, descriptor_.stream.usage_cache_write );
		auto reasoning = optional_int( *parsed, descriptor_.stream.usage_reasoning );

		if ( input || output || cached || written || reasoning ) {
			auto event = chat_event{ };
			event.type = chat_event::kind::usage;
			event.input_tokens = input.value_or( 0 );
			event.output_tokens = output.value_or( 0 );
			event.cached_read_tokens = cached.value_or( 0 );
			event.cache_write_tokens = written.value_or( 0 );
			event.reasoning_tokens = reasoning.value_or( 0 );

			usage_.add( event );

			produced.push_back( std::move( event ) );
		}

		return produced;
	}

	auto delta_applier::finish( ) -> std::vector< chat_event > {
		auto produced = std::vector< chat_event >{ };

		// Completed tool calls, in index order. A call whose arguments never parsed
		// as JSON is still emitted, with its raw fragments, so the caller sees a
		// truncated call rather than a silently missing one.
		for ( auto& call : pending_ ) {
			if ( call.name.empty( ) ) {
				continue;
			}

			auto event = chat_event{ };
			event.type = chat_event::kind::tool_call_delta;
			event.index = call.index;
			event.tool_call_id = call.id;
			event.tool_name = call.name;
			event.args_fragment = call.args_fragments;

			produced.push_back( std::move( event ) );
		}

		auto done = chat_event{ };
		done.type = chat_event::kind::turn_done;
		done.stop_reason = stop_reason_;

		produced.push_back( std::move( done ) );

		return produced;
	}

}
