#include "mcode/net/http_client.hxx"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/err.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#include <wincrypt.h>
#endif

namespace mcode::net {

	namespace {

		namespace beast = boost::beast;
		namespace http = beast::http;
		namespace asio = boost::asio;
		namespace ssl = asio::ssl;

		using tcp = asio::ip::tcp;
		using ssl_stream = ssl::stream< beast::tcp_stream >;

		[[nodiscard]] auto classify( const boost::system::error_code& error_code ) -> errc {
			if ( !error_code ) {
				return errc::ok;
			}

			switch ( error_code.value( ) ) {
				case asio::error::timed_out:
				case asio::error::operation_aborted:
					return errc::cancelled;

				case asio::error::connection_refused:
				case asio::error::host_not_found:
				case asio::error::host_unreachable:
					return errc::io;

				default:
					return error_code.category( ) == asio::error::get_netdb_category( ) ? errc::io
																						: errc::protocol;
			}
		}

		[[nodiscard]] auto lower( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			std::transform( out.begin( ), out.end( ), out.begin( ),
				[]( const unsigned char byte ) { return static_cast< char >( std::tolower( byte ) ); } );

			return out;
		}

		[[nodiscard]] auto is_https( const std::string_view scheme ) -> bool { return scheme == "https"; }

	#if defined( _WIN32 )

		// Windows has no CA file for OpenSSL to find: `set_default_verify_paths`
		// resolves to the build machine's OPENSSLDIR, which does not exist on a
		// user's, so every handshake failed with "certificate verify failed" and
		// HTTPS was unusable. The roots live in the OS store, so they are read
		// from there.
		[[nodiscard]] auto add_windows_roots( X509_STORE* store ) -> bool {
			auto* handle = ::CertOpenSystemStoreW( 0, L"ROOT" );

			if ( handle == nullptr ) {
				return false;
			}

			auto* certificate = static_cast< PCCERT_CONTEXT >( nullptr );
			auto added = std::size_t{ 0 };

			while ( ( certificate = ::CertEnumCertificatesInStore( handle, certificate ) ) != nullptr ) {
				const auto* encoded = certificate->pbCertEncoded;
				auto* parsed = ::d2i_X509( nullptr, &encoded,
					static_cast< long >( certificate->cbCertEncoded ) );

				if ( parsed == nullptr ) {
					continue;
				}

				// A duplicate is reported as an error with nothing added; that is
				// expected across a store this size and is not a failure.
				if ( ::X509_STORE_add_cert( store, parsed ) == 1 ) {
					++added;
				} else {
					::ERR_clear_error( );
				}

				::X509_free( parsed );
			}

			::CertCloseStore( handle, 0 );

			return added > 0;
		}

	#endif

		// Verification stays ON: a verify-disabled build is a security bug, not a
		// convenience. `false` means no trust anchor was found, which is reported
		// rather than silently proceeding.
		[[nodiscard]] auto make_ssl_context( ) -> result< ssl::context > {
			auto context = ssl::context{ ssl::context::tls_client };

			context.set_default_verify_paths( );

		#if defined( _WIN32 )
			if ( !add_windows_roots( ::SSL_CTX_get_cert_store( context.native_handle( ) ) ) ) {
				return std::unexpected( fail( errc::protocol,
					"no trusted root certificates are available; refusing to make an "
					"unverifiable TLS connection" ) );
			}
		#endif

			context.set_verify_mode( ssl::verify_peer );

			return context;
		}

		// One context per process, deliberately never freed.
		//
		// Rebuilding it per request reloads every root in the OS store. Destroying
		// it is worse: a static `ssl::context` is torn down during static
		// destruction, where its ordering against OpenSSL's own cleanup is
		// unspecified, and the process aborted at exit whenever a TLS connection
		// had been made. A process-lifetime context is therefore leaked on purpose.
		[[nodiscard]] auto shared_ssl_context( ) -> ssl::context* {
			static auto* holder = []( ) -> ssl::context* {
				auto built = make_ssl_context( );

				return built ? new ssl::context{ std::move( *built ) } : nullptr;
			}( );

			return holder;
		}

		// The request head, shared by the plain and the TLS path so the two
		// cannot drift. Headers are set in map order, which is also sorted order.
		// The SSE variants add the stream headers, which `send` must not carry.
		template< class Stream >
		auto write_request( Stream& stream, const http_request& request, const url& parsed,
			const bool event_stream ) -> void {
			auto message = http::request< http::string_body >{
				http::string_to_verb( request.method ), parsed.target, 11 };

			message.set( http::field::host, parsed.host );
			message.set( http::field::user_agent, "mcode" );

			if ( event_stream ) {
				message.set( http::field::accept, "text/event-stream" );
				message.set( http::field::cache_control, "no-cache" );
			}

			for ( const auto& [ key, value ] : request.headers ) {
				message.set( key, value );
			}

			if ( !request.body.empty( ) ) {
				message.body( ) = request.body;
				message.prepare_payload( );
			}

			http::write( stream, message );
		}

		template< class Fields >
		[[nodiscard]] auto collect_headers( const http::header< false, Fields >& head )
			-> std::map< std::string, std::string, std::less<> > {
			auto out = std::map< std::string, std::string, std::less<> >{ };

			for ( const auto& field : head ) {
				out.emplace( std::string{ field.name_string( ) }, std::string{ field.value( ) } );
			}

			return out;
		}

		// HTTP/1.1 `Transfer-Encoding: chunked`. The SSE body is read straight off
		// the socket, so the framing has to be removed before the event parser
		// sees it. Left in, a chunk-size line is a line with no colon and is
		// discarded -- but a chunk boundary landing inside an event splits its
		// JSON across two lines, the continuation is discarded as well, and the
		// truncated payload is rejected. Cloudflare fronts the gateway and always
		// chunks, so this is the normal case, not an edge case.
		class chunked_decoder {
		public:
			auto feed( const std::string_view raw ) -> status {
				pending_.append( raw );

				while ( !done_ ) {
					if ( state_ == state::size ) {
						const auto end = pending_.find( '\n' );

						if ( end == std::string::npos ) {
							return { };
						}

						auto line = std::string{ pending_.substr( 0, end ) };

						pending_.erase( 0, end + 1 );

						if ( !line.empty( ) && line.back( ) == '\r' ) {
							line.pop_back( );
						}

						// A chunk extension follows a semicolon and carries no size.
						if ( const auto semicolon = line.find( ';' );
							semicolon != std::string::npos ) {
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

						// The last chunk is followed by an optional trailer, which
						// is not part of the body.
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

					// The CRLF that terminates the chunk data.
					if ( pending_.size( ) < 2 ) {
						return { };
					}

					pending_.erase( 0, 2 );
					state_ = state::size;
				}

				return { };
			}

			[[nodiscard]] auto take_decoded( ) -> std::string {
				return std::exchange( decoded_, std::string{ } );
			}

			[[nodiscard]] auto finished( ) const -> bool { return done_; }

		private:
			enum class state { size, data };

			state state_ = state::size;
			std::string pending_;
			std::string decoded_;
			std::uint64_t remaining_ = 0;
			bool done_ = false;
		};

		[[nodiscard]] auto is_chunked( const std::string_view transfer_encoding ) -> bool {
			return lower( transfer_encoding ).find( "chunked" ) != std::string::npos;
		}

		// Reads the SSE body incrementally. Shared by both transports; the
		// buffered prefix from the header read is drained first, or a server that
		// sends its first event with the response head loses it.
		auto pump_sse( beast::flat_buffer& buffer, std::function< std::size_t( void*, std::size_t ) > read_some,
			sse_parser& parser_state, const std::uint64_t max_response_bytes, const bool chunked ) -> status {
			auto total = std::uint64_t{ 0 };
			auto decoder = chunked_decoder{ };

			const auto deliver = [&]( const std::string_view bytes ) -> status {
				total += bytes.size( );

				if ( total > max_response_bytes ) {
					return std::unexpected(
						fail( errc::protocol, "SSE stream exceeded the response cap" ) );
				}

				parser_state.feed( bytes );

				return { };
			};

			const auto deliver_raw = [&]( const std::string_view bytes ) -> status {
				if ( !chunked ) {
					return deliver( bytes );
				}

				const auto fed = decoder.feed( bytes );

				if ( !fed ) {
					return fed;
				}

				return deliver( decoder.take_decoded( ) );
			};

			if ( buffer.size( ) > 0 ) {
				const auto buffered = static_cast< const char* >( buffer.data( ).data( ) );
				const auto carried = std::string_view{ buffered, buffer.size( ) };

				buffer.consume( buffer.size( ) );

				if ( const auto delivered = deliver_raw( carried ); !delivered ) {
					return delivered;
				}
			}

			auto chunk = std::array< char, 8192 >{ };

			while ( true ) {
				const auto read = read_some( chunk.data( ), chunk.size( ) );

				if ( read == 0 ) {
					break;
				}

				if ( const auto delivered = deliver_raw( std::string_view{ chunk.data( ), read } );
					!delivered ) {
					return delivered;
				}

				if ( decoder.finished( ) ) {
					break;
				}
			}

			parser_state.finish( );

			return { };
		}

	}

	auto parse_url( const std::string_view text ) -> result< url > {
		const auto scheme_end = text.find( "://" );

		if ( scheme_end == std::string_view::npos ) {
			return std::unexpected( fail( errc::protocol, "URL has no scheme: " + std::string{ text } ) );
		}

		auto out = url{ };
		out.scheme = lower( text.substr( 0, scheme_end ) );

		if ( out.scheme != "http" && out.scheme != "https" ) {
			return std::unexpected( fail( errc::unsupported, "unsupported scheme: " + out.scheme ) );
		}

		const auto rest = text.substr( scheme_end + 3 );

		auto authority = rest;
		auto path = std::string_view{ };
		const auto slash = rest.find( '/' );

		if ( slash != std::string_view::npos ) {
			authority = rest.substr( 0, slash );
			path = rest.substr( slash );
		}

		if ( authority.find( '@' ) != std::string_view::npos ) {
			return std::unexpected( fail( errc::protocol,
				"URL contains userinfo, which is not accepted: " + std::string{ authority } ) );
		}

		const auto colon = authority.rfind( ':' );

		if ( colon != std::string_view::npos && authority.find( ']' ) == std::string_view::npos ) {
			out.host = lower( authority.substr( 0, colon ) );
			out.port = std::string{ authority.substr( colon + 1 ) };
		} else {
			out.host = lower( authority );
		}

		if ( out.host.empty( ) ) {
			return std::unexpected( fail( errc::protocol, "URL has an empty host" ) );
		}

		if ( out.host.back( ) == '.' ) {
			return std::unexpected( fail( errc::protocol,
				"URL host has a trailing dot, which is not accepted: " + out.host ) );
		}

		if ( out.port.empty( ) ) {
			out.port = is_https( out.scheme ) ? "443" : "80";
		}

		out.target = path.empty( ) ? "/" : std::string{ path };

		return out;
	}

	auto http_client::send( const http_request& request ) -> result< http_response > {
		auto parsed = parse_url( request.url );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		try {
			auto context = asio::io_context{ };
			auto resolver = tcp::resolver{ context };
			const auto endpoints = resolver.resolve( parsed->host, parsed->port );

			auto out = http_response{ };

			if ( is_https( parsed->scheme ) ) {
				auto* ssl_context = shared_ssl_context( );

				if ( ssl_context == nullptr ) {
					return std::unexpected( fail( errc::protocol,
						"no trusted root certificates are available; refusing to make an "
						"unverifiable TLS connection" ) );
				}

				auto stream = ssl_stream{ context, *ssl_context };

				// Shared hosts route by SNI; without it the handshake picks the
				// wrong certificate and verification fails confusingly.
				if ( !SSL_set_tlsext_host_name( stream.native_handle( ), parsed->host.c_str( ) ) ) {
					return std::unexpected( fail( errc::protocol, "failed to set the SNI host name" ) );
				}

				stream.set_verify_callback( ssl::host_name_verification{ parsed->host } );

				stream.next_layer( ).expires_after( std::chrono::seconds( request.timeout_seconds ) );
				stream.next_layer( ).connect( endpoints );
				stream.handshake( ssl::stream_base::client );

				write_request( stream, request, *parsed, false );

				auto buffer = beast::flat_buffer{ };
				auto parser = http::response_parser< http::string_body >{ };
				parser.body_limit( max_response_bytes_ );
				http::read( stream, buffer, parser );

				auto shutdown_error = boost::system::error_code{ };
				stream.shutdown( shutdown_error );

				out.status = static_cast< int >( parser.get( ).result_int( ) );
				out.body = parser.get( ).body( );
				out.headers = collect_headers( parser.get( ).base( ) );
			} else {
				auto stream = beast::tcp_stream{ context };

				stream.expires_after( std::chrono::seconds( request.timeout_seconds ) );
				stream.connect( endpoints );

				write_request( stream, request, *parsed, false );

				auto buffer = beast::flat_buffer{ };
				auto parser = http::response_parser< http::string_body >{ };
				parser.body_limit( max_response_bytes_ );
				http::read( stream, buffer, parser );

				auto ignored = boost::system::error_code{ };
				stream.socket( ).shutdown( tcp::socket::shutdown_both, ignored );

				out.status = static_cast< int >( parser.get( ).result_int( ) );
				out.body = parser.get( ).body( );
				out.headers = collect_headers( parser.get( ).base( ) );
			}

			return out;
		} catch ( const boost::system::system_error& exception ) {
			return std::unexpected(
				fail( classify( exception.code( ) ), std::string{ "http: " } + exception.what( ) ) );
		}
	}

	auto http_client::stream_sse( const http_request& request, sse_parser::event_callback on_event,
		http_failure* failure ) -> status {
		auto parsed = parse_url( request.url );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		try {
			auto context = asio::io_context{ };
			auto resolver = tcp::resolver{ context };
			const auto endpoints = resolver.resolve( parsed->host, parsed->port );

			// The failure path is the same shape for both transports: read the
			// head, and on a non-2xx read the bounded error body so the caller can
			// classify. The body is what distinguishes a retryable rate limit from
			// a billing one, and `Retry-After` is not re-derivable.
			auto head_ok = true;
			auto status_code = 0;

			auto read_failure_head = [&]( auto& stream, auto& buffer, auto& parser ) {
				http::read_header( stream, buffer, parser );

				status_code = static_cast< int >( parser.get( ).result_int( ) );

				if ( parser.get( ).result( ) == http::status::ok ) {
					return;
				}

				auto body_error = boost::system::error_code{ };
				http::read( stream, buffer, parser, body_error );

				if ( failure != nullptr ) {
					failure->status = status_code;
					failure->body = parser.get( ).body( );
					failure->headers = collect_headers( parser.get( ).base( ) );
				}

				head_ok = false;
			};

			if ( is_https( parsed->scheme ) ) {
				auto* ssl_context = shared_ssl_context( );

				if ( ssl_context == nullptr ) {
					return std::unexpected( fail( errc::protocol,
						"no trusted root certificates are available; refusing to make an "
						"unverifiable TLS connection" ) );
				}

				auto stream = ssl_stream{ context, *ssl_context };

				if ( !SSL_set_tlsext_host_name( stream.native_handle( ), parsed->host.c_str( ) ) ) {
					return std::unexpected( fail( errc::protocol, "failed to set the SNI host name" ) );
				}

				stream.set_verify_callback( ssl::host_name_verification{ parsed->host } );

				stream.next_layer( ).expires_after( std::chrono::seconds( request.timeout_seconds ) );
				stream.next_layer( ).connect( endpoints );
				stream.handshake( ssl::stream_base::client );

				write_request( stream, request, *parsed, true );

				auto buffer = beast::flat_buffer{ };
				auto parser = http::response_parser< http::string_body >{ };
				parser.body_limit( MAX_ERROR_BODY_BYTES );

				read_failure_head( stream, buffer, parser );

				if ( !head_ok ) {
					return std::unexpected( fail( errc::protocol,
						"SSE request returned HTTP " + std::to_string( status_code ) ) );
				}

				auto parser_state = sse_parser{ std::move( on_event ) };

				const auto streamed = pump_sse( buffer,
					[ &stream ]( void* destination, const std::size_t capacity ) {
						return stream.read_some( asio::buffer( destination, capacity ) );
					},
					parser_state, max_response_bytes_,
					is_chunked( parser.get( ).base( )[ http::field::transfer_encoding ] ) );

				// `~ssl_stream` performs a graceful close: it waits for the peer's
				// close_notify. A provider that keeps the connection alive never
				// sends one, so the destructor blocks until the socket times out --
				// and if it throws instead, the throw leaves a destructor and
				// terminates the process. Both were observed against a real gateway:
				// intermittent hangs and 0xC0000409 crashes after a successful
				// stream. The shutdown handshake is skipped and the socket closed.
				::SSL_set_shutdown( stream.native_handle( ),
					SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN );

				auto ignored = boost::system::error_code{ };
				stream.next_layer( ).socket( ).close( ignored );

				return streamed;
			}

			auto stream = beast::tcp_stream{ context };

			stream.expires_after( std::chrono::seconds( request.timeout_seconds ) );
			stream.connect( endpoints );

			write_request( stream, request, *parsed, true );

			auto buffer = beast::flat_buffer{ };
			auto parser = http::response_parser< http::string_body >{ };
			parser.body_limit( MAX_ERROR_BODY_BYTES );

			read_failure_head( stream, buffer, parser );

			if ( !head_ok ) {
				return std::unexpected( fail( errc::protocol,
					"SSE request returned HTTP " + std::to_string( status_code ) ) );
			}

			auto parser_state = sse_parser{ std::move( on_event ) };

			const auto streamed = pump_sse( buffer,
				[ &stream ]( void* destination, const std::size_t capacity ) {
					return stream.read_some( asio::buffer( destination, capacity ) );
				},
				parser_state, max_response_bytes_,
				is_chunked( parser.get( ).base( )[ http::field::transfer_encoding ] ) );

			auto ignored = boost::system::error_code{ };
			stream.socket( ).close( ignored );

			return streamed;
		} catch ( const boost::system::system_error& exception ) {
			if ( exception.code( ) == http::error::end_of_stream || exception.code( ) == asio::error::eof ) {
				return { };
			}

			return std::unexpected(
				fail( classify( exception.code( ) ), std::string{ "sse: " } + exception.what( ) ) );
		} catch ( const std::exception& exception ) {
			// The event callback reports a malformed payload by throwing, so this
			// boundary converts instead of letting it unwind. An exception that
			// escaped here reached no handler and terminated the process: over TLS
			// a stream that splits an event across records hit it every time.
			return std::unexpected(
				fail( errc::protocol, std::string{ "sse: " } + exception.what( ) ) );
		}
	}

}
