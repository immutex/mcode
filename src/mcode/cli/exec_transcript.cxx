#include "mcode/cli/exec.hxx"

#include <algorithm>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "mcode/agent/message_json.hxx"
#include "mcode/support/json.hxx"

namespace mcode::cli {

	// The loop records the assistant message before dispatch, so tool outputs follow it in the log.
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

			// The call id picks which call this answers; a record without one takes the first
			// unanswered.
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


	// What makes a picker usable: an id and a timestamp do not say which session a reader wants.
	auto session_opening_line( const std::filesystem::path& path, const std::size_t max_chars )
		-> std::string {
		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return { };
		}

		// Stops at the first user message, so a picker over many sessions stays cheap.
		auto line = std::string{ };

		while ( std::getline( stream, line ) ) {
			if ( line.find( mcode::agent::MESSAGE_USER_EVENT ) == std::string::npos ) {
				continue;
			}

			auto recorded = json::document::parse( line );

			if ( !recorded ) {
				continue;
			}

			const auto payload = recorded->pointer_raw( "/payload" );

			if ( !payload ) {
				continue;
			}

			auto message = mcode::agent::message_from_json( *payload );

			if ( !message ) {
				continue;
			}

			auto text = std::string{ };

			for ( const auto& block : message->blocks ) {
				if ( block.kind == model::block_kind::text ) {
					text = block.text;

					break;
				}
			}

			// Newlines would break the row it is rendered into.
			for ( auto& character : text ) {
				if ( character == '\n' || character == '\r' || character == '\t' ) {
					character = ' ';
				}
			}

			while ( !text.empty( ) && text.back( ) == ' ' ) {
				text.pop_back( );
			}

			if ( text.size( ) <= max_chars ) {
				return text;
			}

			// Cut on a word boundary when one is close to the limit.
			auto cut = text.rfind( ' ', max_chars );
			text.resize( cut != std::string::npos && cut > max_chars / 2 ? cut : max_chars );
			text += "...";

			return text;
		}

		return { };
	}

}
