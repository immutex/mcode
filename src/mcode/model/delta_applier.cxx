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

	auto delta_applier::pending_for( const int index ) -> pending_call& {
		for ( auto& call : pending_ ) {
			if ( call.index == index ) {
				return call;
			}
		}

		auto call = pending_call{ };
		call.index = index;

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

		const auto raw_index = optional_int( *parsed, descriptor_.stream.tool_call_index )
			.value_or( 0 );

		// the wire-supplied index is untrusted: it addresses an array and is stored as an int.
		if ( raw_index < 0 || raw_index >= MAX_PARALLEL_CALLS ) {
			return std::unexpected( fail( errc::protocol,
				"tool-call index " + std::to_string( raw_index ) + " is outside [0, " +
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
			auto& call = pending_for( static_cast< int >( raw_index ) );

			if ( id_fragment ) {
				call.id += *id_fragment;
			}

			if ( name_fragment ) {
				call.name += *name_fragment;
			}

			if ( args_fragment ) {
				call.args_fragments += *args_fragment;
			}

			// emitted on any fragment, not only once a name is known.
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
