#pragma once

// Shared by the three loopback-server tests (test_http_client, test_model_client,
// test_loop_e2e). They each grew their own copy of this, which meant three places
// to fix when a platform disagrees -- and two of them were wrong on macOS.
//
// Two portability facts this header exists to encode:
//
//   * `SOCKET` is a 64-bit unsigned handle on Windows and an `int` descriptor
//     elsewhere, so the handle is named once rather than cast at every call.
//
//   * The byte-order helpers are FUNCTIONS in glibc but CAST-LIKE MACROS in
//     Darwin's <sys/_endian.h>. `::htonl(x)` is therefore valid on Linux and a
//     syntax error on macOS, where it expands to `::((__uint32_t)(x))`. They must
//     be called unqualified.
//
//   * `send`/`recv` take the length as `int` on Windows and `size_t` on POSIX.
//     Passing a `size_t` cast to `int` is a -Wsign-conversion error on POSIX with
//     warnings-as-errors, so the conversion lives here behind one branch.

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

	// Winsock needs a process-wide init that POSIX does not. Idempotent, so every
	// server may call it.
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

	// Unqualified on purpose: `htonl` is a macro on Darwin and `::htonl` does not
	// compile there. A cast-like macro expands to an expression, which cannot
	// follow a leading `::`.
	[[nodiscard]] inline auto host_to_network_long( const std::uint32_t value ) -> std::uint32_t {
		return htonl( value );
	}

	[[nodiscard]] inline auto network_to_host_short( const std::uint16_t value ) -> std::uint16_t {
		return ntohs( value );
	}

	// Returns false when the peer closed or the call failed; the loopback servers
	// treat either as "stop sending".
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

	// The number of bytes read, 0 on an orderly close, or -1 on error. Same shape
	// as `recv` so callers can compare directly.
	//
	// Deliberately not `[[nodiscard]]`: the loopback servers drain the client's
	// request without inspecting it, and forcing a cast at those sites would be a
	// no-op that hides nothing. The callers that do care compare the result.
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
