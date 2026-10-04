#include <catch2/catch_test_macros.hpp>

#include "lua.h"

#include "mcode/ext/lua_host.hxx"

using namespace mcode;

TEST_CASE( "a runaway chunk is stopped by the default time budget", "[lua_host]" ) {
	auto host = lua_host::create( lua_host_options{ .extension_name = "runaway", .module_loader = { } } );

	REQUIRE( static_cast< bool >( host ) );

	if ( !host ) {
		FAIL( "could not create the VM: " << host.error( ).msg );
	}

	auto ran = host->run( "local n = 0\nwhile true do n = n + 1 end", "=(runaway)" );

	REQUIRE_FALSE( static_cast< bool >( ran ) );

	if ( ran ) {
		return;
	}

	REQUIRE( ran.error( ).msg.find( "time budget" ) != std::string::npos );
	REQUIRE( host->time_breaches( ) > 0 );
}

TEST_CASE( "run leaves the thread stack empty", "[lua_host]" ) {
	auto host = lua_host::create( lua_host_options{ .extension_name = "stack", .module_loader = { } } );

	REQUIRE( static_cast< bool >( host ) );

	if ( !host ) {
		FAIL( "could not create the VM: " << host.error( ).msg );
	}

	REQUIRE( static_cast< bool >( host->run( "return 1, 2, 3", "=(stack)" ) ) );

	auto* state = host->raw( );

	REQUIRE( state != nullptr );

	if ( state == nullptr ) {
		return;
	}

	// a discarded return value left on the stack would grow it once per call, without bound
	for ( auto attempt = 0; attempt < 16; ++attempt ) {
		REQUIRE( static_cast< bool >( host->run( "return 1, 2, 3", "=(stack)" ) ) );
		REQUIRE( lua_gettop( state ) == 0 );
	}
}
