#pragma once

#if defined( _WIN32 )
#include <windows.h>
#endif

#include <string>

#include "mcode/core/error.hxx"

namespace mcode::platform {

#if defined( _WIN32 )

	// INBOUND grants the server GENERIC_READ, OUTBOUND grants it GENERIC_WRITE.
	enum class sandbox_pipe_direction { child_reads, child_writes };

	inline constexpr std::size_t WINDOWS_MESSAGE_BUFFER_CHARS = 0;

	// The system's own text for a Win32 status, minus the trailing newline.
	[[nodiscard]] inline auto last_error_message( const DWORD error ) -> std::string {
		auto* buffer = LPWSTR{ nullptr };

		const auto length = ::FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0,
			reinterpret_cast< LPWSTR >( &buffer ), 0, nullptr );

		if ( length == 0 || buffer == nullptr ) {
			return "windows error " + std::to_string( error );
		}

		const auto text = std::wstring{ buffer, length };
		::LocalFree( buffer );

		auto utf8 = std::string{ };
		const auto bytes = ::WideCharToMultiByte( CP_UTF8, 0, text.c_str( ),
			static_cast< int >( text.size( ) ), nullptr, 0, nullptr, nullptr );

		if ( bytes > 0 ) {
			utf8.resize( static_cast< std::size_t >( bytes ) );
			::WideCharToMultiByte( CP_UTF8, 0, text.c_str( ),
				static_cast< int >( text.size( ) ), utf8.data( ), bytes, nullptr, nullptr );
		}

		while ( !utf8.empty( ) && ( utf8.back( ) == '\r' || utf8.back( ) == '\n' ) ) {
			utf8.pop_back( );
		}

		return utf8;
	}

	[[nodiscard]] inline auto fail_win( const std::string& what, const DWORD error )
		-> mcode::error {
		return mcode::fail( mcode::errc::io,
			what + " failed: " + last_error_message( error ) );
	}

#endif

}

namespace mcode::platform::detail {

#if defined( _WIN32 )

	using mcode::platform::fail_win;
	using mcode::platform::last_error_message;
	using mcode::platform::sandbox_pipe_direction;

#endif

}
