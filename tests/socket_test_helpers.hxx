#pragma once

// `SOCKET` is a 64-bit handle on Windows, an `int` descriptor elsewhere.
// `htonl`/`ntohs` are cast-like macros on Darwin: `::htonl` does not compile there.
// `send`/`recv` take the length as `int` on Windows and `size_t` on POSIX.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mcode::test {

#if defined( _WIN32 )
	using socket_handle = SOCKET;
	inline constexpr socket_handle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
	using socket_handle = int;
	inline constexpr socket_handle INVALID_SOCKET_HANDLE = -1;
#endif

	// Winsock needs a process-wide init that POSIX does not.
	inline auto ensure_sockets( ) -> void {
	#if defined( _WIN32 )
		static const auto started = []( ) {
			auto data = WSADATA{ };

			return ::WSAStartup( MAKEWORD( 2, 2 ), &data ) == 0;
		}( );

		(void)started;
	#endif
	}

	inline auto close_socket( const socket_handle handle ) -> void {
	#if defined( _WIN32 )
		::closesocket( handle );
	#else
		::close( handle );
	#endif
	}

	inline auto shutdown_socket( const socket_handle handle ) -> void {
	#if defined( _WIN32 )
		::shutdown( handle, SD_BOTH );
	#else
		::shutdown( handle, SHUT_RDWR );
	#endif
	}

	// unqualified on purpose: `htonl` is a macro on Darwin, where `::htonl` does not compile.
	[[nodiscard]] inline auto host_to_network_long( const std::uint32_t value ) -> std::uint32_t {
		return htonl( value );
	}

	[[nodiscard]] inline auto network_to_host_short( const std::uint16_t value ) -> std::uint16_t {
		return ntohs( value );
	}

	// false on peer close or call failure; the loopback servers treat either as "stop sending".
	[[nodiscard]] inline auto send_bytes( const socket_handle handle, const std::string_view data )
		-> bool {
		if ( data.empty( ) ) {
			return true;
		}

	#if defined( _WIN32 )
		const auto sent = ::send( handle, data.data( ), static_cast< int >( data.size( ) ), 0 );

		return sent != SOCKET_ERROR;
	#else
		const auto sent = ::send( handle, data.data( ), data.size( ), 0 );

		return sent >= 0;
	#endif
	}

	// bytes read, 0 on orderly close, -1 on error; deliberately not `[[nodiscard]]`.
	inline auto receive_bytes( const socket_handle handle, const std::span< char > buffer )
		-> std::ptrdiff_t {
		if ( buffer.empty( ) ) {
			return 0;
		}

	#if defined( _WIN32 )
		return ::recv( handle, buffer.data( ), static_cast< int >( buffer.size( ) ), 0 );
	#else
		return ::recv( handle, buffer.data( ), buffer.size( ), 0 );
	#endif
	}

}
