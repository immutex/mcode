#include "mcode/cli/exec.hxx"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mcode/agent/message_json.hxx"
#include "mcode/support/json.hxx"

namespace mcode::cli {

	// Rebuilds the conversation from a session log. Only the events that carry a
	// message or a tool output contribute; the activity events (`tool.call`,
	// `run.end`, `session.*`) are diagnostics and are not part of the transcript.
	//
	// A tool output is attached to the assistant message that requested it, which
	// is where the model expects it: the loop records the assistant message
	// before dispatch, so the outputs follow it in the log.
	[[nodiscard]] auto restore_transcript( const event_log& log )
		-> std::vector< model::message > {
		auto history = std::vector< model::message >{ };

		for ( const auto& recorded : log.events( ) ) {
			if ( recorded.kind == mcode::agent::MESSAGE_USER_EVENT
				|| recorded.kind == mcode::agent::MESSAGE_ASSISTANT_EVENT ) {
				auto message = mcode::agent::message_from_json( recorded.payload_json );

				if ( message ) {
					history.push_back( std::move( *message ) );
				}

				continue;
			}

			if ( recorded.kind != mcode::agent::TOOL_RESULT_EVENT ) {
				continue;
			}

			auto document = json::document::parse( recorded.payload_json );

			if ( !document ) {
				continue;
			}

			auto content = document->get_string( "content" );

			if ( !content ) {
				continue;
			}

			// The result rides on the last assistant message, as a tool block
			// carrying the id the call used. One message can carry several calls,
			// so the id decides which one this answers -- matching the first call
			// instead attached every result to it and left the others unanswered.
			//
			// A record with no id is from a session written before the id was
			// recorded; there the first call still lacking a result is the best
			// available guess, which is also what makes several results on one
			// message land on their own calls in order.
			if ( history.empty( ) ) {
				continue;
			}

			auto& last = history.back( );

			if ( last.speaker != model::role::assistant ) {
				continue;
			}

			const auto call_id = document->get_string( "call_id" );

			const auto has_result = [ &last ]( const std::string& id ) {
				return std::any_of( last.blocks.begin( ), last.blocks.end( ),
					[ &id ]( const model::block& other ) {
						return other.kind == model::block_kind::tool_result &&
							other.tool_call_id == id;
					} );
			};

			auto target = std::string{ };

			for ( const auto& block : last.blocks ) {
				if ( block.kind != model::block_kind::tool_call ) {
					continue;
				}

				if ( call_id && block.tool_call_id != *call_id ) {
					continue;
				}

				if ( has_result( block.tool_call_id ) ) {
					continue;
				}

				target = block.tool_call_id;

				break;
			}

			if ( target.empty( ) ) {
				continue;
			}

			auto result = model::block{ };
			result.kind = model::block_kind::tool_result;
			result.tool_call_id = std::move( target );
			result.result_json = *content;
			last.blocks.push_back( std::move( result ) );
		}

		return history;
	}

}
