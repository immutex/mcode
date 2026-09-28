// Measurement spine: cold start, idle RSS, per-extension load, per-extension
// memory, and hook dispatch cost.
//
// Reports `key=value` lines so CI can gate on them and a human can diff runs.
// Every number here is a budget in docs/01 or docs/26; if a number moves, the
// doc that owns the budget moves with it.
//
// Usage: mcode_bench [extensions] [tools-per-ext] [handlers]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#endif

#include "mcode/ext/lua_host.hxx"
#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"

namespace {

	using clock_type = std::chrono::steady_clock;

	auto current_rss_kb( ) -> unsigned long long {
	#if defined( _WIN32 )
		PROCESS_MEMORY_COUNTERS counters{};

		if ( GetProcessMemoryInfo( GetCurrentProcess( ), &counters, sizeof( counters ) ) == 0 ) {
			return 0;
		}

		return counters.WorkingSetSize / 1024;
	#else
		auto* file = std::fopen( "/proc/self/statm", "r" );

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
	#endif
	}

	auto peak_rss_kb( ) -> unsigned long long {
	#if defined( _WIN32 )
		PROCESS_MEMORY_COUNTERS counters{};

		if ( GetProcessMemoryInfo( GetCurrentProcess( ), &counters, sizeof( counters ) ) == 0 ) {
			return 0;
		}

		return counters.PeakWorkingSetSize / 1024;
	#else
		auto* file = std::fopen( "/proc/self/status", "r" );

		if ( file == nullptr ) {
			return 0;
		}

		char line[ 256 ]{ };
		unsigned long long peak = 0;

		while ( std::fgets( line, sizeof( line ), file ) != nullptr ) {
			if ( std::strncmp( line, "VmHWM:", 6 ) == 0 ) {
				std::sscanf( line + 6, "%llu", &peak );

				break;
			}
		}

		std::fclose( file );

		return peak;
	#endif
	}

	auto milliseconds_since( const clock_type::time_point start ) -> double {
		return std::chrono::duration_cast< std::chrono::nanoseconds >(
			clock_type::now( ) - start ).count( ) / 1'000'000.0;
	}

	// The full frozen surface from docs/18, registered with stubs. Measuring a
	// partial surface would understate load cost, and the number has to match
	// what a real extension actually receives.
	auto register_api_surface( mcode::lua_host& host ) -> bool {
		constexpr const char* NAMES[] = {
			"api_version", "capabilities", "ext_name", "tool_register", "tool_unregister",
			"cmd_register", "on", "off", "emit", "defer", "timer_at", "timer_every",
			"log_debug", "log_info", "log_warn", "log_error", "notify", "cfg_get",
			"session_snapshot", "session_fork", "spawn", "net_get", "net_search",
			"fs_read", "fs_write", "skill_register", "mcp_register", "context_add",
		};

		for ( const auto* name : NAMES ) {
			auto registered = host.register_host_function( name,
				[]( const std::string_view ) -> mcode::result< std::string > {
					return std::string{ };
				} );

			if ( !registered ) {
				std::printf( "  api registration failed for %s: %s\n", name,
					registered.error( ).msg.c_str( ) );

				return false;
			}
		}

		return true;
	}

	auto build_extension_source( const int tool_count ) -> std::string {
		auto source = std::string{ "local registered = 0\n" };

		for ( auto index = 0; index < tool_count; ++index ) {
			source += "mcode.tool_register('tool_" + std::to_string( index ) + "')\n";
			source += "registered = registered + 1\n";
		}

		source += "mcode.log_info('loaded', registered)\n";
		source += "return registered";

		return source;
	}

	auto build_dispatch_source( const int handler_count ) -> std::string {
		auto source = std::string{ "local handlers = {}\n" };

		for ( auto index = 0; index < handler_count; ++index ) {
			source += "handlers[" + std::to_string( index + 1 ) + "] = function(payload)\n";
			source += "  return payload .. '.'\n";
			source += "end\n";
		}

		source += "function dispatch(payload)\n";
		source += "  local out = payload\n";
		source += "  for index = 1, #handlers do\n";
		source += "    out = handlers[index](out)\n";
		source += "  end\n";
		source += "  return out\n";
		source += "end\n";
		source += "return #handlers";

		return source;
	}

}

auto main( int argument_count, char** arguments ) -> int {
	const auto extension_count = ( argument_count > 1 ) ? std::atoi( arguments[ 1 ] ) : 50;
	const auto tools_per_extension = ( argument_count > 2 ) ? std::atoi( arguments[ 2 ] ) : 8;
	const auto dispatch_iterations = ( argument_count > 3 ) ? std::atoi( arguments[ 3 ] ) : 20'000;

	const auto process_start = clock_type::now( );
	const auto baseline_rss = current_rss_kb( );

	auto hosts = std::vector< mcode::lua_host >{ };
	hosts.reserve( static_cast< std::size_t >( extension_count ) );

	const auto source = build_extension_source( tools_per_extension );

	// --- B2: extension load -------------------------------------------------
	const auto load_start = clock_type::now( );
	auto load_failures = 0;

	for ( auto index = 0; index < extension_count; ++index ) {
		auto options = mcode::lua_host_options{ };
		options.extension_name = "bench_" + std::to_string( index );

		auto host = mcode::lua_host::create( std::move( options ) );

		if ( !host ) {
			++load_failures;

			continue;
		}

		if ( !register_api_surface( *host ) ) {
			++load_failures;

			continue;
		}

		auto ran = host->run( source, "=(bench)" );

		if ( !ran ) {
			std::printf( "  extension %d failed to run: %s\n", index, ran.error( ).msg.c_str( ) );
			++load_failures;

			continue;
		}

		hosts.push_back( std::move( *host ) );
	}

	const auto load_ms = milliseconds_since( load_start );
	const auto loaded = static_cast< int >( hosts.size( ) );

	// --- B3: per-extension memory ------------------------------------------
	auto total_bytes = std::uint64_t{ 0 };
	auto peak_bytes = std::uint64_t{ 0 };

	for ( const auto& host : hosts ) {
		total_bytes += host.bytes_allocated( );
		peak_bytes = std::max( peak_bytes, host.peak_bytes_allocated( ) );
	}

	// --- B4: hook dispatch --------------------------------------------------
	auto dispatch_per_call_us = std::vector< double >{ };

	for ( const int handler_count : { 0, 1, 10, 50 } ) {
		auto options = mcode::lua_host_options{ };
		options.extension_name = "dispatch";
		options.time_limit = std::chrono::milliseconds{ 0 };

		auto host = mcode::lua_host::create( std::move( options ) );

		if ( !host ) {
			continue;
		}

		if ( !register_api_surface( *host ) ) {
			continue;
		}

		auto dispatch_source = build_dispatch_source( handler_count );

		if ( !host->run( dispatch_source, "=(dispatch)" ) ) {
			continue;
		}

		const auto started = clock_type::now( );

		for ( auto index = 0; index < dispatch_iterations; ++index ) {
			auto result = host->call_global( "dispatch", "x" );

			if ( !result ) {
				break;
			}
		}

		const auto total_ms = milliseconds_since( started );
		dispatch_per_call_us.push_back( total_ms * 1000.0 / dispatch_iterations );

		std::printf( "dispatch_handlers_%d_us_per_call=%.4f\n", handler_count,
			dispatch_per_call_us.back( ) );
	}

	// --- fixed cost of a bare VM, no API surface, no extension code ---------
	// Isolates what `luaL_newstate` + `luaL_openlibs` + the sandbox costs, so a
	// per-extension budget can distinguish the fixed VM from the extension's own
	// allocations.
	{
		auto bare = mcode::lua_host::create( { .extension_name = "bare" } );

		if ( bare ) {
			// Force sealing without running anything, which is what installs the
			// sandbox and the thread.
			auto sealed = bare->run( "return 0", "=(bare)" );

			if ( sealed ) {
				std::printf( "bare_vm_bytes=%llu\n",
					static_cast< unsigned long long >( bare->bytes_allocated( ) ) );
				std::printf( "bare_vm_peak_bytes=%llu\n",
					static_cast< unsigned long long >( bare->peak_bytes_allocated( ) ) );
			}
		}
	}

	// --- D4: escape hatch vs the declarative path ---------------------------
	// The escape hatch moves per-event work into the VM, so its cost is the thing
	// that decides whether a provider may use it. Measured as per-event overhead
	// for the same stream, both ways.
	{
		const auto declarative_json = std::string{ R"({
			"name": "bench-declarative",
			"endpoint": "https://example.invalid/v1/chat/completions",
			"stream": { "text_delta": "/choices/0/delta/content" }
		})" };

		const auto hatch_json = std::string{ R"({
			"name": "bench-hatch",
			"endpoint": "https://example.invalid/v1/chat/completions",
			"on_event": true
		})" };

		const auto payload = std::string{ R"({"choices":[{"delta":{"content":"x"}}]})" };

		auto descriptor = mcode::model::descriptor_from_json( declarative_json );

		if ( descriptor ) {
			auto applier = mcode::model::delta_applier{ *descriptor };

			const auto started = clock_type::now( );
			auto total = std::size_t{ 0 };

			for ( auto index = 0; index < dispatch_iterations; ++index ) {
				auto produced = applier.feed( "message", payload );

				if ( produced ) {
					total += produced->size( );
				}
			}

			std::printf( "provider_declarative_us_per_event=%.4f\n",
				milliseconds_since( started ) * 1000.0 / dispatch_iterations );
			std::printf( "provider_declarative_events=%llu\n",
				static_cast< unsigned long long >( total ) );
		}

		auto hatch = mcode::model::descriptor_from_json( hatch_json );

		if ( !hatch ) {
			std::printf( "provider_escape_hatch_error=descriptor:%s\n", hatch.error( ).msg.c_str( ) );
		}

		if ( hatch ) {
			// The hatch parses in Lua, using the VM's own string and table work --
			// the shape a real exotic provider would need. This is the cost the
			// declarative path exists to avoid.
			auto host = mcode::lua_host::create( { .extension_name = "hatch" } );

			if ( !host ) {
				std::printf( "provider_escape_hatch_error=vm:%s\n", host.error( ).msg.c_str( ) );
			}

			if ( host ) {
				// The delimiter is LUA, not the default: the Lua source contains `)"`
				// inside a string pattern, which would terminate a plain R"( )" early.
				//
				// The handler returns the extracted text as a string. A real host would
				// marshal a table, but the crossing measured here -- one payload in, one
				// canonical value out, plus the Lua work -- is the same cost, and the
				// string return keeps the event count honest.
				auto ran = host->run( R"LUA(
					function on_event(payload)
						local content = string.match(payload, '"content":"([^"]*)"')
						if content then
							return "text_delta:" .. content
						end
						return ""
					end
				)LUA", "=(hatch)" );

				if ( !ran ) {
					std::printf( "provider_escape_hatch_error=lua:%s\n", ran.error( ).msg.c_str( ) );
				}

				if ( ran ) {
					auto applier = mcode::model::delta_applier{ *hatch };
					auto lua_host_pointer = &*host;

					applier.set_escape_hatch( [lua_host_pointer]( const std::string_view event_name,
						const std::string_view data ) -> mcode::result< std::vector< mcode::model::chat_event > > {
						(void)event_name;

						auto result = lua_host_pointer->call_global( "on_event", data );

						if ( !result ) {
							return std::unexpected( result.error( ) );
						}

						auto events = std::vector< mcode::model::chat_event >{ };

						if ( result->starts_with( "text_delta:" ) ) {
							auto event = mcode::model::chat_event{ };
							event.type = mcode::model::chat_event::kind::text_delta;
							event.text = result->substr( 11 );

							events.push_back( std::move( event ) );
						}

						return events;
					} );

					const auto started = clock_type::now( );
					auto total = std::size_t{ 0 };

					for ( auto index = 0; index < dispatch_iterations; ++index ) {
						auto produced = applier.feed( "message", payload );

						if ( produced ) {
							total += produced->size( );
						}
					}

					std::printf( "provider_escape_hatch_us_per_event=%.4f\n",
						milliseconds_since( started ) * 1000.0 / dispatch_iterations );
					std::printf( "provider_escape_hatch_events=%llu\n",
						static_cast< unsigned long long >( total ) );
				}
			}
		}
	}

	// --- report -------------------------------------------------------------
	const auto rss_now = current_rss_kb( );

	std::printf( "extensions=%d\n", loaded );
	std::printf( "tools_per_extension=%d\n", tools_per_extension );
	std::printf( "load_failures=%d\n", load_failures );
	std::printf( "load_total_ms=%.3f\n", load_ms );
	std::printf( "load_per_ext_us=%.1f\n", loaded > 0 ? ( load_ms * 1000.0 / loaded ) : 0.0 );
	std::printf( "ext_bytes_total=%llu\n", static_cast< unsigned long long >( total_bytes ) );
	std::printf( "ext_bytes_mean=%llu\n",
		static_cast< unsigned long long >( loaded > 0 ? total_bytes / static_cast< std::uint64_t >( loaded ) : 0 ) );
	std::printf( "ext_bytes_peak=%llu\n", static_cast< unsigned long long >( peak_bytes ) );
	std::printf( "rss_baseline_kb=%llu\n", baseline_rss );
	std::printf( "rss_after_kb=%llu\n", rss_now );
	std::printf( "rss_delta_kb=%lld\n",
		static_cast< long long >( rss_now ) - static_cast< long long >( baseline_rss ) );
	std::printf( "rss_peak_kb=%llu\n", peak_rss_kb( ) );
	std::printf( "process_uptime_ms=%.3f\n", milliseconds_since( process_start ) );

	return load_failures != 0 ? 1 : 0;
}
