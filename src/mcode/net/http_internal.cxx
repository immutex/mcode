#include "mcode/net/http_internal.hxx"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

namespace mcode::net::detail {

	// A chunk-size line is hex digits, an optional extension and a CRLF. RFC 9112
	// puts no bound on it, and the SSE response cap counts DECODED bytes, so a
	// line that never terminates grows the pending buffer while the cap reads
	// zero. This is the bound that closes that.
	inline constexpr std::size_t MAX_CHUNK_SIZE_LINE_BYTES = 4 * 1024;

	auto chunked_decoder::feed( const std::string_view raw ) -> status {
		pending_.append( raw );

		while ( !done_ ) {
			if ( state_ == state::size ) {
				const auto end = pending_.find( '\n' );

				if ( end == std::string::npos ) {
					// A size line has no bound of its own, and the response cap
					// counts decoded bytes -- which stay zero while the size line
					// never terminates. Without this a server could grow the
					// buffer indefinitely by sending a hex line that never ends.
					if ( pending_.size( ) > MAX_CHUNK_SIZE_LINE_BYTES ) {
						return std::unexpected( fail( errc::protocol,
							"chunk size line exceeded its bound" ) );
					}

					return { };
				}

				auto line = std::string{ pending_.substr( 0, end ) };

				pending_.erase( 0, end + 1 );

				if ( !line.empty( ) && line.back( ) == '\r' ) {
					line.pop_back( );
				}

				if ( const auto semicolon = line.find( ';' ); semicolon != std::string::npos ) {
					line.resize( semicolon );
				}

				auto size = std::uint64_t{ 0 };
				const auto* begin = line.data( );
				const auto* finish = begin + line.size( );
				const auto parsed = std::from_chars( begin, finish, size, 16 );

				if ( parsed.ec != std::errc{ } || parsed.ptr != finish ) {
					return std::unexpected(
						fail( errc::protocol, "malformed chunk size in the SSE body" ) );
				}

				// A zero-size chunk is followed by an optional trailer, not body data.
				if ( size == 0 ) {
					done_ = true;

					return { };
				}

				remaining_ = size;
				state_ = state::data;

				continue;
			}

			const auto take = std::min< std::uint64_t >( remaining_, pending_.size( ) );

			decoded_.append( pending_, 0, static_cast< std::size_t >( take ) );
			pending_.erase( 0, static_cast< std::size_t >( take ) );
			remaining_ -= take;

			if ( remaining_ != 0 ) {
				return { };
			}

			if ( pending_.size( ) < 2 ) {
				return { };
			}

			// The two bytes are a CRLF. Erasing them unexamined desynchronises
			// the whole stream when a lenient server terminates a chunk with a
			// bare LF: the erase eats the LF and the next chunk-size line's
			// first digit, so every later chunk is mis-sized and the damaged
			// bytes are handed on as body.
			if ( pending_[ 0 ] != '\r' || pending_[ 1 ] != '\n' ) {
				return std::unexpected( fail( errc::protocol,
					"chunk is not terminated by CRLF" ) );
			}

			pending_.erase( 0, 2 );
			state_ = state::size;
		}

		return { };
	}

	auto chunked_decoder::take_decoded( ) -> std::string {
		return std::exchange( decoded_, std::string{ } );
	}

	auto chunked_decoder::finished( ) const -> bool { return done_; }

	auto is_chunked( const std::string_view transfer_encoding ) -> bool {
		auto lowered = std::string{ transfer_encoding };

		std::transform( lowered.begin( ), lowered.end( ), lowered.begin( ),
			[]( const unsigned char byte ) { return static_cast< char >( std::tolower( byte ) ); } );

		return lowered.find( "chunked" ) != std::string::npos;
	}

}
