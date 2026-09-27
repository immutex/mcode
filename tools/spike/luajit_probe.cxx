#if defined( _WIN32 )
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

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
		return resident * 4;  // pages of 4 KiB
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
		lua_pushinteger( state, static_cast< lua_Integer >( std::strlen( text ) ) );
		return 1;
	}

	auto run_source( lua_State* state, const char* source, const char* name ) -> int {
		if ( luaL_loadbuffer( state, source, std::strlen( source ), name ) != 0 ) {
			return -1;
		}
		return lua_pcall( state, 0, 1, 0 );
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

	// Match mcode's policy: JIT off by default. Pass "jitoff" as arg 3 to compare
	// against the JIT-enabled run.
	const bool jit_off = ( argument_count > 3 ) && ( std::strcmp( arguments[ 3 ], "jitoff" ) == 0 );
	if ( jit_off ) {
		luaL_dostring( state, "jit.off()" );
	}

	lua_pushcfunction( state, host_call );
	lua_setglobal( state, "host_call" );

	for ( int index = 0; index < extension_count; ++index ) {
		if ( run_source( state, EXTENSION_SOURCE, "=ext" ) != 0 ) {
			std::printf( "load error: %s\n", lua_tostring( state, -1 ) );
			return 1;
		}
		lua_pop( state, 1 );
	}

	const auto after_load = std::chrono::steady_clock::now( );
	const auto load_ms = std::chrono::duration_cast< std::chrono::microseconds >(
		after_load - started ).count( ) / 1000.0;

	auto boundary_source = std::string{ "local n = 0\nfor index = 1, " };
	boundary_source += std::to_string( iterations );
	boundary_source += " do\n  n = n + host_call('abcdefgh')\nend\nreturn n";

	const auto boundary_started = std::chrono::steady_clock::now( );
	if ( run_source( state, boundary_source.c_str( ), "=boundary" ) != 0 ) {
		std::printf( "boundary error: %s\n", lua_tostring( state, -1 ) );
		return 1;
	}
	lua_pop( state, 1 );
	const auto boundary_ms = std::chrono::duration_cast< std::chrono::microseconds >(
		std::chrono::steady_clock::now( ) - boundary_started ).count( ) / 1000.0;

	auto probe = [&]( const char* source ) -> int {
		const int outcome = run_source( state, source, "=probe" );
		if ( outcome == 0 ) {
			lua_pop( state, 1 );
			return 1;
		}
		lua_pop( state, 1 );
		return 0;
	};

	const int ffi_reachable = probe( "local ok, ffi = pcall(require, 'ffi') if ok and ffi then return ffi end error('absent')" );

	std::printf( "vm=luajit\n" );
	std::printf( "jit=%s\n", jit_off ? "off" : "on" );
	std::printf( "extensions=%d\n", extension_count );
	std::printf( "load_ms=%.3f\n", load_ms );
	std::printf( "per_ext_us=%.1f\n", extension_count > 0 ? ( load_ms * 1000.0 / extension_count ) : 0.0 );
	std::printf( "boundary_ms=%.3f\n", boundary_ms );
	std::printf( "rss_baseline_kb=%llu\n", baseline_rss );
	std::printf( "rss_after_kb=%llu\n", current_rss_kb( ) );
	std::printf( "rss_delta_kb=%lld\n", static_cast< long long >( current_rss_kb( ) ) - static_cast< long long >( baseline_rss ) );
	std::printf( "rss_peak_kb=%llu\n", peak_rss_kb( ) );
	std::printf( "probe_ffi_reachable=%d\n", ffi_reachable );

	lua_close( state );
	return 0;
}
