#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#if defined( _WIN32 )
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

using namespace mcode;

namespace {

	// ctest redirects stdout, so CI only exercises the negative branch.
	auto terminal_attached( ) -> bool {
	#if defined( _WIN32 )
		return _isatty( _fileno( stdout ) ) != 0;
	#else
		return ::isatty( STDOUT_FILENO ) != 0;
	#endif
	}

}

TEST_CASE( "the seven platform seams exist and report honestly", "[platform]" ) {
	const auto pty_supported = platform::pty_session::supported( );

	auto pty = platform::pty_session::spawn( "sh", { } );

	if ( pty_supported ) {
		REQUIRE( static_cast< bool >( pty ) );
	} else {
		REQUIRE_FALSE( static_cast< bool >( pty ) );

		if ( !pty ) {
			REQUIRE( pty.error( ).code == errc::unsupported );
		}
	}

	// the seam reports per capability, not one merged claim.
	REQUIRE_FALSE( platform::sandbox_mechanism( ).empty( ) );

	if ( platform::sandbox_capability_level( ) == platform::sandbox_capability::unavailable ) {
		REQUIRE( platform::sandbox_network_level( )
			== platform::sandbox_network_support::unavailable );
	}

	// on POSIX `kill(0, 0)` and `kill(-1, 0)` succeed, so an unvalidated pid reports as alive.
	REQUIRE( platform::process_is_alive( 0 ) == false );
	REQUIRE( platform::process_is_alive( 0xFFFFFFFFu ) == false );
	REQUIRE( platform::process_is_alive( 0x80000000u ) == false );

#if defined( _WIN32 )
	REQUIRE( platform::process_is_alive( ::GetCurrentProcessId( ) ) );
#else
	REQUIRE( platform::process_is_alive( static_cast< std::uint64_t >( ::getpid( ) ) ) );
#endif

	auto canonical = platform::canonicalize( std::filesystem::current_path( ) );
	REQUIRE( static_cast< bool >( canonical ) );

	auto temp = platform::temp_directory( );
	REQUIRE( static_cast< bool >( temp ) );
	REQUIRE( std::filesystem::exists( *temp ) );

#if defined( _WIN32 ) || defined( __APPLE__ )
	// true even on a case-sensitive volume: the boundary compares case-folded.
	REQUIRE( platform::case_insensitive_paths( ) );
#endif

#if defined( _WIN32 )
	auto extended = platform::to_extended_path( "C:\\Windows" );
	REQUIRE( extended.wstring( ).starts_with( L"\\\\?\\" ) );
#endif

#if !defined( _WIN32 ) && !defined( __APPLE__ )
	REQUIRE_FALSE( platform::case_insensitive_paths( ) );
#endif

	auto config_path = platform::app_data_path( platform::data_kind::config );
	REQUIRE( static_cast< bool >( config_path ) );
	REQUIRE( config_path->string( ).find( "mcode" ) != std::string::npos );

	auto cache_path = platform::app_data_path( platform::data_kind::cache );
	REQUIRE( static_cast< bool >( cache_path ) );

	// a size is reported only when there is a terminal, and it is positive.
	auto size = platform::terminal_size( );

	if ( terminal_attached( ) ) {
		REQUIRE( static_cast< bool >( size ) );

		if ( size ) {
			REQUIRE( size->first > 0 );
			REQUIRE( size->second > 0 );
		}
	} else {
		REQUIRE_FALSE( static_cast< bool >( size ) );
	}

	REQUIRE_FALSE( platform::file_watcher::supported( ) );
	auto watcher = platform::file_watcher::create( *temp, false );
	REQUIRE_FALSE( static_cast< bool >( watcher ) );
	REQUIRE( watcher.error( ).code == errc::unsupported );
}
