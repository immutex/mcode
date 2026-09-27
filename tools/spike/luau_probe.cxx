#if defined( _WIN32 )
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

namespace {

#if defined( _WIN32 )
	auto current_rss_kb( ) -> unsigned long long {
		PROCESS_MEMORY_COUNTERS counters{};
		if ( GetProcessMemoryInfo( GetCurrentProcess( ), &counters, sizeof( counters ) ) == 0 ) {
			return 0;
		}
		return counters.WorkingSetSize / 1024;
	}

	auto peak_rss_kb( ) -> unsigned long long {
		PROCESS_MEMORY_COUNTERS counters{};
		if ( GetProcessMemoryInfo( GetCurrentProcess( ), &counters, sizeof( counters ) ) == 0 ) {
			return 0;
		}
		return counters.PeakWorkingSetSize / 1024;
	}
#else
	auto current_rss_kb( ) -> unsigned long long {
		std::FILE* file = std::fopen( "/proc/self/statm", "r" );
		if ( file == nullptr ) {
			return 0;
		}
		unsigned long long total = 0;
		unsigned long long resident = 0;
		if ( std::fscanf( file, "%llu %llu", &total, &resident ) != 2 ) {
			std::fclose( file );
			return 0;
		}
		std::fclose( file );
		return resident * 4;
	}

	auto peak_rss_kb( ) -> unsigned long long {
		std::FILE* file = std::fopen( "/proc/self/status", "r" );
		if ( file == nullptr ) {
			return 0;
		}
		char line[ 256 ]{};
		unsigned long long peak = 0;
		while ( std::fgets( line, sizeof( line ), file ) != nullptr ) {
			if ( std::strncmp( line, "VmHWM:", 6 ) == 0 ) {
				std::sscanf( line + 6, "%llu", &peak );
				break;
			}
		}
		std::fclose( file );
		return peak;
	}
#endif

	int host_call( lua_State* state ) {
		const char* text = luaL_checkstring( state, 1 );
		lua_pushinteger( state, static_cast< int >( std::strlen( text ) ) );
		return 1;
	}

	// Compiles and runs on `thread`. Leaves exactly the result on `thread`'s
	// stack on success. Returns 0 on success.
	auto run_on( lua_State* thread, const char* source, const char* name ) -> int {
		size_t bytecode_size = 0;
		char* bytecode = luau_compile( source, std::strlen( source ), nullptr, &bytecode_size );
		if ( bytecode == nullptr ) {
			return -1;
		}
		const int loaded = luau_load( thread, name, bytecode, bytecode_size, 0 );
		std::free( bytecode );
		if ( loaded != 0 ) {
			return -2;
		}
		return lua_pcall( thread, 0, 1, 0 );
	}

	const char* EXTENSION_SOURCE = R"(
		local ext = { tools = {}, name = "synthetic" }
		for index = 1, 20 do
			ext.tools[index] = { name = "tool_" .. index, description = "synthetic tool " .. index }
		end
		return ext
	)";

}

auto main( int argument_count, char** arguments ) -> int {
	const int extension_count = ( argument_count > 1 ) ? std::atoi( arguments[ 1 ] ) : 10;
	const int iterations = ( argument_count > 2 ) ? std::atoi( arguments[ 2 ] ) : 200000;

	const auto baseline_rss = current_rss_kb( );
	const auto started = std::chrono::steady_clock::now( );

	lua_State* state = luaL_newstate( );
	luaL_openlibs( state );

	// Globals must be registered BEFORE luaL_sandbox: sandboxing makes _G
	// readonly, and lua_setglobal on a readonly table panics outside a pcall.
	lua_pushcfunction( state, host_call, "host_call" );
	lua_setglobal( state, "host_call" );

	luaL_sandbox( state );

	// Each extension gets its own sandboxed thread. The thread value sits on the
	// parent stack, so it is popped after each use.
	for ( int index = 0; index < extension_count; ++index ) {
		lua_State* thread = lua_newthread( state );
		luaL_sandboxthread( thread );
		const int outcome = run_on( thread, EXTENSION_SOURCE, "=ext" );
		if ( outcome != 0 ) {
			std::printf( "load error: %s\n", lua_tostring( thread, -1 ) );
			return 1;
		}
		lua_pop( thread, 1 );
		lua_pop( state, 1 );
	}

	const auto after_load = std::chrono::steady_clock::now( );
	const auto load_ms = std::chrono::duration_cast< std::chrono::microseconds >(
		after_load - started ).count( ) / 1000.0;

	auto boundary_source = std::string{ "local n = 0\nfor index = 1, " };
	boundary_source += std::to_string( iterations );
	boundary_source += " do\n  n = n + host_call('abcdefgh')\nend\nreturn n";

	const auto boundary_started = std::chrono::steady_clock::now( );
	{
		lua_State* thread = lua_newthread( state );
		luaL_sandboxthread( thread );
		const int outcome = run_on( thread, boundary_source.c_str( ), "=boundary" );
		if ( outcome != 0 ) {
			std::printf( "boundary error: %s\n", lua_tostring( thread, -1 ) );
			return 1;
		}
		lua_pop( thread, 1 );
		lua_pop( state, 1 );
	}
	const auto boundary_ms = std::chrono::duration_cast< std::chrono::microseconds >(
		std::chrono::steady_clock::now( ) - boundary_started ).count( ) / 1000.0;

	// Each probe returns 1 when the escape SUCCEEDED (which is a failure for us).
	auto probe = [&]( const char* source ) -> int {
		lua_State* thread = lua_newthread( state );
		luaL_sandboxthread( thread );
		const int outcome = run_on( thread, source, "=probe" );
		int escaped = 0;
		if ( outcome == 0 ) {
			escaped = lua_toboolean( thread, -1 ) != 0 ? 1 : 0;
			lua_pop( thread, 1 );
		}
		lua_pop( state, 1 );
		return escaped;
	};

	std::printf( "vm=luau\n" );
	std::printf( "extensions=%d\n", extension_count );
	std::printf( "load_ms=%.3f\n", load_ms );
	std::printf( "per_ext_us=%.1f\n", extension_count > 0 ? ( load_ms * 1000.0 / extension_count ) : 0.0 );
	std::printf( "boundary_ms=%.3f\n", boundary_ms );
	std::printf( "rss_baseline_kb=%llu\n", baseline_rss );
	std::printf( "rss_after_kb=%llu\n", current_rss_kb( ) );
	std::printf( "rss_delta_kb=%lld\n", static_cast< long long >( current_rss_kb( ) ) - static_cast< long long >( baseline_rss ) );
	std::printf( "rss_peak_kb=%llu\n", peak_rss_kb( ) );
	std::printf( "probe_global_write_escaped=%d\n",
		probe( "return (rawset(_G, 'injected', 1) ~= nil)" ) );
	std::printf( "probe_io_reachable=%d\n", probe( "return io ~= nil" ) );
	std::printf( "probe_os_execute_reachable=%d\n", probe( "return (os and os.execute) ~= nil" ) );
	std::printf( "probe_loadstring_reachable=%d\n", probe( "return loadstring ~= nil" ) );
	std::printf( "probe_setmetatable_escaped=%d\n",
		probe( "local ok = pcall(setmetatable, _G, {}) return ok" ) );

	lua_close( state );
	return 0;
}
