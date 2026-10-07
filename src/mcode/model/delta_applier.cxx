#include "mcode/model/delta_applier.hxx"

#include <algorithm>
#include <optional>

#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

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

		auto event_allowed( const std::string_view name, const std::vector< std::string >& gate )
			-> bool {
			if ( gate.empty( ) ) {
				return true;
			}

			return std::find( gate.begin( ), gate.end( ), name ) != gate.end( );
		}

	}

	delta_applier::delta_applier( const provider_descriptor& descriptor )
		: descriptor_( descriptor ) { }

	delta_applier::delta_applier( provider_descriptor&& descriptor )
		: descriptor_( std::move( descriptor ) ) { }

	auto delta_applier::set_escape_hatch( escape_hatch callback ) -> void {
		escape_ = std::move( callback );
	}

	auto delta_applier::resolve_pending( const std::optional< std::int64_t > wire_index,
		const std::string_view id_fragment, const std::string_view name_fragment ) -> pending_call& {
		// The index is optional on the wire and unreliable when present: absent on some
		// gateways, pinned at zero on others, so it cannot identify a call on its own. A
		// fragment that contradicts what the candidate already holds belongs to the next call
		// instead - which is exactly what a constant index looks like across two calls.
		//
		// The id is decisive because it is unique per call and arrives whole. Names repeat
		// across calls, so a differing name only starts a new call when neither value is a
		// prefix of the other, which distinguishes a genuine second call from a name split
		// across fragments.
		const auto is_extension = []( const std::string_view stored, const std::string_view value ) {
			return stored.starts_with( value ) || value.starts_with( stored );
		};

		const auto contradicts = [ & ]( const pending_call& call ) {
			if ( !id_fragment.empty( ) && !call.id.empty( ) && call.id != id_fragment ) {
				return true;
			}

			if ( !name_fragment.empty( ) && !call.name.empty( ) &&
				!is_extension( call.name, name_fragment ) ) {
				return true;
			}

			return false;
		};

		const auto candidate = [ & ]( ) -> pending_call* {
			if ( wire_index ) {
				for ( auto& call : pending_ ) {
					if ( call.wire_index == *wire_index ) {
						return &call;
					}
				}

				return nullptr;
			}

			// Without an index the newest call is the only candidate.
			return pending_.empty( ) ? nullptr : &pending_.back( );
		};

		if ( auto* found = candidate( ); found != nullptr && !contradicts( *found ) ) {
			return *found;
		}

		auto call = pending_call{ };
		call.ordinal = static_cast< int >( pending_.size( ) );
		call.wire_index = wire_index.value_or( -1 );

		pending_.push_back( std::move( call ) );

		return pending_.back( );
	}

	auto delta_applier::feed( const std::string_view event_name, const std::string_view data )
		-> result< std::vector< chat_event > > {
		auto produced = std::vector< chat_event >{ };

		if ( descriptor_.escape_hatch ) {
			if ( !escape_ ) {
				return std::unexpected( fail( errc::protocol,
					"provider '" + descriptor_.name +
					"' uses the on_event escape hatch but no hatch is installed" ) );
			}

			return escape_( event_name, data );
		}

		// trailing events after the terminal one would overwrite state that is already final.
		if ( terminal_seen_ ) {
			return produced;
		}

		for ( const auto& terminal : descriptor_.stream.terminal_events ) {
			if ( event_name == terminal ) {
				terminal_seen_ = true;

				return produced;
			}
		}

		// the [DONE] sentinel is a bare payload, not JSON.
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

		// a mid-stream failure is terminal and arrives as an ordinary event, not an HTTP status.
		if ( !descriptor_.stream.error_message.empty( ) ) {
			if ( auto message = optional_string( *parsed, descriptor_.stream.error_message ) ) {
				auto detail = std::string{ };

				if ( !descriptor_.stream.error_code.empty( ) ) {
					if ( auto code = optional_string( *parsed, descriptor_.stream.error_code ) ) {
						detail = " (" + *code + ")";
					}
				}

				return std::unexpected( fail( errc::protocol,
					"provider reported a mid-stream error: " + *message + detail ) );
			}
		}

		const auto text_applies = event_allowed( event_name, descriptor_.stream.text_events );
		const auto tool_applies = event_allowed( event_name, descriptor_.stream.tool_call_events );

		if ( text_applies ) {
			if ( auto text = optional_string( *parsed, descriptor_.stream.text_delta ) ) {
				if ( !text->empty( ) ) {
					auto event = chat_event{ };
					event.type = chat_event::kind::text_delta;
					event.text = *text;

					produced.push_back( std::move( event ) );
				}
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

		// The index is optional on the wire and is unreliable when present: absent on some
		// gateways, pinned at zero on others. It selects the call a fragment extends; when it
		// disagrees with what has already accumulated, the fragment starts a new call instead.
		const auto wire_index = optional_int( *parsed, descriptor_.stream.tool_call_index );

		if ( wire_index && ( *wire_index < 0 || *wire_index >= MAX_PARALLEL_CALLS ) ) {
			return std::unexpected( fail( errc::protocol,
				"tool-call index " + std::to_string( *wire_index ) + " is outside [0, " +
				std::to_string( MAX_PARALLEL_CALLS ) + ")" ) );
		}

		auto id_fragment = tool_applies
			? optional_string( *parsed, descriptor_.stream.tool_call_id )
			: std::nullopt;
		auto name_fragment = tool_applies
			? optional_string( *parsed, descriptor_.stream.tool_call_name )
			: std::nullopt;
		auto args_fragment = tool_applies
			? optional_string( *parsed, descriptor_.stream.tool_call_args )
			: std::nullopt;

		if ( id_fragment || name_fragment || args_fragment ) {
			auto& call = resolve_pending( wire_index,
				id_fragment.value_or( std::string{ } ),
				name_fragment.value_or( std::string{ } ) );

			// A fragment that extends what is accumulated is that same value re-sent, so it
			// replaces rather than duplicates; a genuine continuation is appended.
			const auto absorb = []( std::string& target, const std::string& fragment ) {
				if ( fragment.empty( ) ) {
					return;
				}

				if ( fragment.starts_with( target ) ) {
					target = fragment;
				} else {
					target += fragment;
				}
			};

			absorb( call.id, id_fragment.value_or( std::string{ } ) );
			absorb( call.name, name_fragment.value_or( std::string{ } ) );
			absorb( call.args_fragments, args_fragment.value_or( std::string{ } ) );

			// emitted on any fragment, not only once a name is known.
			auto event = chat_event{ };
			event.type = chat_event::kind::tool_call_delta;
			event.index = call.ordinal;
			event.tool_call_id = call.id;
			event.tool_name = call.name;
			event.args_fragment = args_fragment.value_or( std::string{ } );

			produced.push_back( std::move( event ) );
		}

		if ( auto reason = optional_string( *parsed, descriptor_.stream.finish_reason ) ) {
			stop_reason_ = *reason;
		}

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

		for ( auto& call : pending_ ) {
			if ( call.name.empty( ) && call.id.empty( ) && call.args_fragments.empty( ) ) {
				continue;
			}

			auto event = chat_event{ };
			event.type = chat_event::kind::tool_call_delta;
			event.index = call.ordinal;
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
