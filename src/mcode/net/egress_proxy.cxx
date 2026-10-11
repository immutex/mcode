#include "mcode/net/egress_proxy.hxx"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "mcode/support/text.hxx"

namespace mcode::net {

	namespace asio = boost::asio;
	using tcp = asio::ip::tcp;

	namespace {

		constexpr std::size_t MAX_REQUEST_BYTES = 8192;
		constexpr std::size_t RELAY_CHUNK = 16384;
		constexpr int SOCKS_VERSION = 5;
		constexpr int SOCKS_NO_AUTH = 0;
		constexpr int SOCKS_CONNECT = 1;
		constexpr int SOCKS_SUCCEEDED = 0;
		constexpr int SOCKS_NOT_ALLOWED = 2;
		constexpr unsigned char SOCKS_ATYP_IPV4 = 1;
		constexpr std::uint16_t PLAIN_HTTP_PORT = 80;

		struct target {
			std::string host;
			std::uint16_t port = 0;
		};

		// `CONNECT host:port`, or absolute-form `GET http://host/path`, which a plain-HTTP
		// client sends.
		auto parse_http_request( const std::string& request, target& out ) -> bool {
			const auto space = request.find( ' ' );

			if ( space == std::string::npos ) {
				return false;
			}

			const auto method = request.substr( 0, space );
			const auto rest = request.find( ' ', space + 1 );

			if ( rest == std::string::npos ) {
				return false;
			}

			auto authority = request.substr( space + 1, rest - space - 1 );

			if ( method == "CONNECT" ) {
				// `authority` is already host:port.
			} else {
				const auto scheme = authority.find( "://" );

				if ( scheme == std::string::npos ) {
					return false;
				}

				authority = authority.substr( scheme + 3 );

				const auto slash = authority.find( '/' );

				if ( slash != std::string::npos ) {
					authority = authority.substr( 0, slash );
				}

				if ( method != "GET" && method != "POST" && method != "HEAD" ) {
					return false;
				}
			}

			const auto colon = authority.rfind( ':' );

			if ( colon == std::string::npos || colon == 0 ) {
				// A missing port is a scheme default that is not stated, so refuse rather
				// than guess.
				return false;
			}

			out.host = authority.substr( 0, colon );

			auto port = 0;

			const auto parsed = std::from_chars( authority.data( ) + colon + 1,
				authority.data( ) + authority.size( ), port );

			if ( parsed.ec != std::errc{ } || port <= 0 || port > 65535 ) {
				return false;
			}

			out.port = static_cast< std::uint16_t >( port );

			return !out.host.empty( );
		}

	}

	struct egress_proxy::implementation {
		egress_policy policy;
		asio::io_context context;
		std::unique_ptr< tcp::acceptor > http_acceptor;
		std::unique_ptr< tcp::acceptor > socks_acceptor;
		std::thread http_worker;
		std::thread socks_worker;

		std::atomic< bool > stopping{ false };
		std::atomic< std::uint64_t > denied{ 0 };
		std::atomic< std::uint64_t > allowed{ 0 };

		// Resolves once and dials the first address the policy allows, so a DNS rebind cannot
		// swap the address between the check and the connect.
		auto connect_upstream( const target& destination, tcp::socket& upstream ) -> bool {
			auto error = boost::system::error_code{ };
			auto resolver = tcp::resolver{ context };
			const auto endpoints = resolver.resolve( destination.host,
				std::to_string( destination.port ), error );

			if ( error ) {
				return false;
			}

			for ( const auto& endpoint : endpoints ) {
				if ( egress_policy::address_is_denied(
						endpoint.endpoint( ).address( ).to_string( ) ) ) {
					continue;
				}

				upstream.connect( endpoint, error );

				if ( !error ) {
					return true;
				}
			}

			return false;
		}

		// One thread per direction: a single sequential pump would deadlock on any protocol
		// where the server speaks first, because nothing has been forwarded upstream yet.
		static auto pump( tcp::socket& from, tcp::socket& to ) -> void {
			auto buffer = std::vector< char >( RELAY_CHUNK );
			auto error = boost::system::error_code{ };

			while ( !error ) {
				const auto read = from.read_some( asio::buffer( buffer ), error );

				if ( error || read == 0 ) {
					break;
				}

				asio::write( to, asio::buffer( buffer.data( ), read ), error );
			}

			// Half-close rather than close: the peer may still have data to send.
			to.shutdown( tcp::socket::shutdown_send, error );
		}

		static auto relay( tcp::socket& client, tcp::socket& upstream ) -> void {
			auto forward = std::thread{ [ &client, &upstream ] { pump( client, upstream ); } };

			pump( upstream, client );

			if ( forward.joinable( ) ) {
				forward.join( );
			}
		}

		// Reads until the end of the request headers: one `read_some` may return a partial
		// request, and a truncated `CONNECT` line would answer 400 to a request that was
		// merely split. The deadline matters because `stop` joins this thread, so a client
		// that connects and says nothing must not block the worker forever.
		auto read_request_head( tcp::socket& client, std::string& out ) -> bool {
			constexpr std::string_view TERMINATOR = "\r\n\r\n";
			constexpr auto DEADLINE = std::chrono::seconds{ 10 };

			auto error = boost::system::error_code{ };
			client.non_blocking( true, error );

			auto buffer = std::vector< char >( MAX_REQUEST_BYTES );
			const auto give_up_at = std::chrono::steady_clock::now( ) + DEADLINE;

			while ( out.size( ) < MAX_REQUEST_BYTES ) {
				if ( std::chrono::steady_clock::now( ) >= give_up_at ) {
					break;
				}

				const auto read = client.read_some( asio::buffer( buffer ), error );

				if ( read > 0 ) {
					out.append( buffer.data( ), read );
					error.clear( );

					if ( out.find( TERMINATOR ) != std::string::npos ) {
						client.non_blocking( false, error );

						return true;
					}

					continue;
				}

				if ( error == asio::error::would_block || error == asio::error::try_again ) {
					error.clear( );
					std::this_thread::sleep_for( std::chrono::milliseconds{ 2 } );

					continue;
				}

				return false;
			}

			client.non_blocking( false, error );

			return out.find( TERMINATOR ) != std::string::npos;
		}

		auto handle_http( tcp::socket client ) -> void {
			auto error = boost::system::error_code{ };
			auto request = std::string{ };

			if ( !read_request_head( client, request ) ) {
				return;
			}

			auto destination = target{ };

			if ( !parse_http_request( request, destination ) ) {
				++denied;

				auto reply = std::string{ "HTTP/1.1 400 Bad Request\r\n\r\n" };
				asio::write( client, asio::buffer( reply ), error );

				return;
			}

			// The decision is made before any byte is relayed, so a refused host sees a 403
			// and nothing else. The port is restricted as well as the host.
			if ( policy.decide( destination.host, destination.port )
					== egress_decision::deny
				|| ( destination.port != PLAIN_HTTP_PORT
					&& !connect_port_allowed( destination.port ) ) ) {
				++denied;

				auto reply = std::string{ "HTTP/1.1 403 Forbidden\r\n\r\n" };
				asio::write( client, asio::buffer( reply ), error );

				return;
			}

			auto upstream = tcp::socket{ context };

			if ( !connect_upstream( destination, upstream ) ) {
				++denied;

				auto reply = std::string{ "HTTP/1.1 502 Bad Gateway\r\n\r\n" };
				asio::write( client, asio::buffer( reply ), error );

				return;
			}

			++allowed;

			auto established = std::string{ "HTTP/1.1 200 Connection Established\r\n\r\n" };
			asio::write( client, asio::buffer( established ), error );

			relay( client, upstream );
		}

		auto handle_socks( tcp::socket client ) -> void {
			auto error = boost::system::error_code{ };
			auto greeting = std::array< unsigned char, 3 >{ };

			if ( asio::read( client, asio::buffer( greeting ), error ) != greeting.size( ) ) {
				return;
			}

			if ( greeting[ 0 ] != SOCKS_VERSION ) {
				return;
			}

			auto methods = std::vector< unsigned char >( greeting[ 1 ] );

			if ( asio::read( client, asio::buffer( methods ), error ) != methods.size( ) ) {
				return;
			}

			const auto no_auth = std::array< unsigned char, 2 >{
				SOCKS_VERSION, SOCKS_NO_AUTH };

			asio::write( client, asio::buffer( no_auth ), error );

			auto request = std::array< unsigned char, 4 >{ };

			if ( asio::read( client, asio::buffer( request ), error ) != request.size( ) ) {
				return;
			}

			if ( request[ 1 ] != SOCKS_CONNECT ) {
				return;
			}

			auto destination = target{ };

			if ( request[ 3 ] == 1 ) {
				auto address = std::array< unsigned char, 4 >{ };

				if ( asio::read( client, asio::buffer( address ), error ) != address.size( ) ) {
					return;
				}

				destination.host = std::to_string( address[ 0 ] ) + "."
					+ std::to_string( address[ 1 ] ) + "." + std::to_string( address[ 2 ] )
					+ "." + std::to_string( address[ 3 ] );
			} else if ( request[ 3 ] == 3 ) {
				auto length = std::array< unsigned char, 1 >{ };

				if ( asio::read( client, asio::buffer( length ), error ) != 1 ) {
					return;
				}

				auto name = std::vector< char >( length[ 0 ] );

				if ( asio::read( client, asio::buffer( name ), error ) != name.size( ) ) {
					return;
				}

				destination.host = std::string{ name.data( ), name.size( ) };
			} else {
				return;
			}

			auto port = std::array< unsigned char, 2 >{ };

			if ( asio::read( client, asio::buffer( port ), error ) != port.size( ) ) {
				return;
			}

			destination.port = static_cast< std::uint16_t >(
				( static_cast< std::uint16_t >( port[ 0 ] ) << 8 ) | port[ 1 ] );

			// VER, REP, RSV, ATYP; BND.ADDR and BND.PORT stay zero. RFC 1928 defines no
			// ATYP 0, so a reply that leaves it unset is malformed and a strict client
			// rejects it.
			auto reply = std::array< unsigned char, 10 >{
				SOCKS_VERSION, 0, 0, SOCKS_ATYP_IPV4 };

			if ( policy.decide( destination.host, destination.port )
				== egress_decision::deny ) {
				++denied;
				reply[ 1 ] = SOCKS_NOT_ALLOWED;

				asio::write( client, asio::buffer( reply ), error );

				return;
			}

			auto upstream = tcp::socket{ context };

			if ( !connect_upstream( destination, upstream ) ) {
				++denied;
				reply[ 1 ] = SOCKS_NOT_ALLOWED;

				asio::write( client, asio::buffer( reply ), error );

				return;
			}

			++allowed;
			reply[ 1 ] = SOCKS_SUCCEEDED;

			asio::write( client, asio::buffer( reply ), error );

			relay( client, upstream );
		}

		// Non-blocking accept with a poll step: closing a socket another thread is blocked in
		// `accept()` on does not reliably wake it on Linux.
		auto accept_loop( tcp::acceptor& acceptor, const bool is_http ) -> void {
			while ( !stopping.load( ) ) {
				auto error = boost::system::error_code{ };
				auto client = tcp::socket{ context };

				acceptor.accept( client, error );

				if ( error ) {
					if ( stopping.load( ) ) {
						return;
					}

					if ( error == asio::error::would_block
						|| error == asio::error::try_again ) {
						std::this_thread::sleep_for( std::chrono::milliseconds{ 5 } );
					}

					continue;
				}

				if ( is_http ) {
					handle_http( std::move( client ) );
				} else {
					handle_socks( std::move( client ) );
				}
			}
		}
	};

	auto egress_proxy::start( egress_policy policy ) -> result< egress_proxy > {
		auto proxy = egress_proxy{ };
		proxy.self_ = new implementation{ };
		proxy.self_->policy = std::move( policy );

		auto error = boost::system::error_code{ };

		proxy.self_->http_acceptor = std::make_unique< tcp::acceptor >( proxy.self_->context );
		proxy.self_->http_acceptor->open( tcp::v4( ), error );
		proxy.self_->http_acceptor->bind( tcp::endpoint{ asio::ip::make_address( "127.0.0.1" ), 0 },
			error );
		proxy.self_->http_acceptor->listen( asio::socket_base::max_listen_connections, error );

		if ( error ) {
			delete proxy.self_;
			proxy.self_ = nullptr;

			return std::unexpected( fail( errc::io,
				"cannot bind the egress proxy: " + error.message( ) ) );
		}

		proxy.self_->socks_acceptor = std::make_unique< tcp::acceptor >( proxy.self_->context );
		proxy.self_->socks_acceptor->open( tcp::v4( ), error );
		proxy.self_->socks_acceptor->bind( tcp::endpoint{ asio::ip::make_address( "127.0.0.1" ), 0 },
			error );
		proxy.self_->socks_acceptor->listen( asio::socket_base::max_listen_connections, error );

		if ( error ) {
			delete proxy.self_;
			proxy.self_ = nullptr;

			return std::unexpected( fail( errc::io,
				"cannot bind the SOCKS listener: " + error.message( ) ) );
		}

		proxy.self_->http_acceptor->non_blocking( true, error );
		proxy.self_->socks_acceptor->non_blocking( true, error );

		return proxy;
	}

	auto egress_proxy::serve( ) -> void {
		if ( self_ == nullptr ) {
			return;
		}

		// Two blocking accept loops need two threads; returns immediately.
		self_->http_worker = std::thread{ [ this ] {
			self_->accept_loop( *self_->http_acceptor, true );
		} };

		self_->socks_worker = std::thread{ [ this ] {
			self_->accept_loop( *self_->socks_acceptor, false );
		} };
	}

	auto egress_proxy::stop( ) -> void {
		if ( self_ == nullptr ) {
			return;
		}

		self_->stopping.store( true );

		// Closing the acceptor is what wakes a blocked `accept`.
		auto error = boost::system::error_code{ };
		self_->http_acceptor->close( error );
		self_->socks_acceptor->close( error );

		if ( self_->http_worker.joinable( ) ) {
			self_->http_worker.join( );
		}

		if ( self_->socks_worker.joinable( ) ) {
			self_->socks_worker.join( );
		}
	}

	auto egress_proxy::http_port( ) const noexcept -> std::uint16_t {
		if ( self_ == nullptr ) {
			return 0;
		}

		auto error = boost::system::error_code{ };
		const auto endpoint = self_->http_acceptor->local_endpoint( error );

		return error ? 0 : endpoint.port( );
	}

	auto egress_proxy::socks_port( ) const noexcept -> std::uint16_t {
		if ( self_ == nullptr ) {
			return 0;
		}

		auto error = boost::system::error_code{ };
		const auto endpoint = self_->socks_acceptor->local_endpoint( error );

		return error ? 0 : endpoint.port( );
	}

	auto egress_proxy::environment( ) const -> std::string {
		const auto http = "http://127.0.0.1:" + std::to_string( http_port( ) );
		const auto socks = "socks5://127.0.0.1:" + std::to_string( socks_port( ) );

		return "HTTP_PROXY=" + http + ";HTTPS_PROXY=" + http + ";ALL_PROXY=" + socks
			+ ";NO_PROXY=localhost,127.0.0.1";
	}

	auto egress_proxy::denied_count( ) const noexcept -> std::uint64_t {
		return self_ != nullptr ? self_->denied.load( ) : 0;
	}

	auto egress_proxy::allowed_count( ) const noexcept -> std::uint64_t {
		return self_ != nullptr ? self_->allowed.load( ) : 0;
	}

	egress_proxy::egress_proxy( egress_proxy&& other ) noexcept : self_( other.self_ ) {
		other.self_ = nullptr;
	}

	auto egress_proxy::operator=( egress_proxy&& other ) noexcept -> egress_proxy& {
		if ( this != &other ) {
			this->~egress_proxy( );
			self_ = other.self_;
			other.self_ = nullptr;
		}

		return *this;
	}

	egress_proxy::~egress_proxy( ) {
		if ( self_ != nullptr ) {
			stop( );

			delete self_;
			self_ = nullptr;
		}
	}

}
