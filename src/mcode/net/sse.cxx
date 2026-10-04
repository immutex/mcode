#include "mcode/net/sse.hxx"

#include <string>
#include <utility>

#include "mcode/support/json.hxx"

namespace mcode::net {

	namespace {

		auto split_field( const std::string_view line, std::string_view& field, std::string_view& value )
			-> void {
			const auto colon = line.find( ':' );

			if ( colon == std::string_view::npos ) {
				field = line;
				value = { };

				return;
			}

			field = line.substr( 0, colon );
			value = line.substr( colon + 1 );

			if ( !value.empty( ) && value.front( ) == ' ' ) {
				value.remove_prefix( 1 );
			}
		}

	}

	auto sse_parser::feed( const std::string_view chunk ) -> void {
		auto content = chunk;

		// A CRLF split across chunks: the CR ended the line already, so this LF is its pair.
		if ( pending_cr_terminator_ && !content.empty( ) && content.front( ) == '\n' ) {
			content.remove_prefix( 1 );
		}

		pending_cr_terminator_ = false;
		buffer_.append( content );

		auto position = std::size_t{ 0 };

		while ( position < buffer_.size( ) ) {
			const auto terminator = buffer_.find_first_of( "\r\n", position );

			if ( terminator == std::string::npos ) {
				break;
			}

			process_line( std::string_view{ buffer_ }.substr( position, terminator - position ) );

			position = terminator + 1;

			if ( buffer_[ terminator ] == '\r' ) {
				if ( position < buffer_.size( ) ) {
					if ( buffer_[ position ] == '\n' ) {
						++position;
					}
				} else {
					pending_cr_terminator_ = true;
				}
			}
		}

		if ( position > 0 ) {
			buffer_.erase( 0, position );
		}

		if ( buffer_.size( ) > MAX_LINE_BYTES ) {
			buffer_.clear( );
		}
	}

	auto sse_parser::process_line( const std::string_view line ) -> void {
		if ( line.empty( ) ) {
			dispatch( );

			return;
		}

		if ( line.front( ) == ':' ) {
			return;
		}

		auto field = std::string_view{ };
		auto value = std::string_view{ };
		split_field( line, field, value );

		if ( field == "data" ) {
			if ( saw_data_ ) {
				current_.data += '\n';
			}

			current_.data.append( value );
			saw_data_ = true;
		} else if ( field == "event" ) {
			current_.event.assign( value );
		} else if ( field == "id" ) {
			if ( value.find( '\0' ) == std::string_view::npos ) {
				current_.id.assign( value );
			}
		} else if ( field == "retry" ) {
			current_.retry.assign( value );
		}
	}

	auto sse_parser::dispatch( ) -> void {
		if ( !saw_data_ && current_.event.empty( ) && current_.id.empty( ) ) {
			return;
		}

		if ( current_.event.empty( ) ) {
			current_.event = "message";
		}

		++events_parsed_;

		auto event = std::move( current_ );
		current_ = sse_event{ };
		saw_data_ = false;

		if ( on_event_ ) {
			on_event_( std::move( event ) );
		}
	}

	auto sse_parser::finish( ) -> void {
		// the spec discards an event still open at EOF: only a terminated blank line dispatches.
		buffer_.clear( );
		current_ = sse_event{ };
		saw_data_ = false;
	}

	auto extract_delta_text( const std::string_view data_payload ) -> result< std::string > {
		if ( data_payload == "[DONE]" ) {
			return std::string{ };
		}

		auto doc = json::document::parse( data_payload );

		if ( !doc ) {
			return std::unexpected( doc.error( ) );
		}

		if ( auto direct = doc->get_string( "delta" ) ) {
			return *direct;
		}

		if ( auto text = doc->pointer( "/choices/0/delta/content" ) ) {
			if ( text->size( ) >= 2 && text->front( ) == '"' && text->back( ) == '"' ) {
				return text->substr( 1, text->size( ) - 2 );
			}

			return *text;
		}

		return std::string{ };
	}

}
