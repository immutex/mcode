// Measurement spine: cold start, idle RSS, per-extension load, per-extension
// memory, and hook dispatch cost.
//
// Reports `key=value` lines so CI can gate on them and a human can diff runs.
// Every number here is a documented budget; if a number moves, the
// doc that owns the budget moves with it.
//
// Usage: mcode_bench [extensions] [tools-per-ext] [handlers]

#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>

#include <psapi.h>
#elif defined( __APPLE__ )
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#include "mcode/ext/lua_host.hxx"
#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"

namespace {

	using clock_type = std::chrono::steady_clock;

	// `01-north-star.md` bounds cold start at 15 ms. Referenced here rather than redefined:
	// the doc owns the budget, this only measures against it.
	inline constexpr double COLD_START_BUDGET_MS = 15.0;

#if !defined( _WIN32 )
	auto page_size_bytes( ) -> long {
		static const auto cached = ::sysconf( _SC_PAGESIZE );

		return cached > 0 ? cached : 4096;
	}
#endif

	// Milliseconds since the process was created, from the OS, so the measurement includes
	// dynamic-loader and C-runtime start-up that a timer started in `main` cannot see. That
	// is the whole point of the cold-start budget: `01` bounds the time to a usable process.
	auto process_age_ms( ) -> double {
	#if defined( _WIN32 )
		FILETIME created{};
		FILETIME exited{};
		FILETIME kernel{};
		FILETIME user{};

		if ( GetProcessTimes( GetCurrentProcess( ), &created, &exited, &kernel, &user ) == 0 ) {
			return -1.0;
		}

		ULARGE_INTEGER created_ticks{};
		created_ticks.LowPart = created.dwLowDateTime;
		created_ticks.HighPart = created.dwHighDateTime;

		FILETIME now{};
		GetSystemTimeAsFileTime( &now );

		ULARGE_INTEGER now_ticks{};
		now_ticks.LowPart = now.dwLowDateTime;
		now_ticks.HighPart = now.dwHighDateTime;

		// FILETIME is 100-nanosecond intervals, so the difference is 100ns units.
		return static_cast< double >( now_ticks.QuadPart - created_ticks.QuadPart ) / 10'000.0;
	#elif defined( __APPLE__ )
		// No /proc on macOS. `sysctl` gives the process's start timeval directly, so no
		// system-uptime subtraction is needed and no clock-tick conversion is involved.
		auto info = kinfo_proc{ };
		auto size = sizeof( info );
		auto name = std::array< int, 4 >{ CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid( ) };

		if ( ::sysctl( name.data( ), name.size( ), &info, &size, nullptr, 0 ) != 0 ) {
			return -1.0;
		}

		auto now = timeval{ };

		if ( ::gettimeofday( &now, nullptr ) != 0 ) {
			return -1.0;
		}

		const auto started = static_cast< double >( info.kp_proc.p_starttime.tv_sec ) +
			static_cast< double >( info.kp_proc.p_starttime.tv_usec ) / 1'000'000.0;
		const auto current = static_cast< double >( now.tv_sec ) +
			static_cast< double >( now.tv_usec ) / 1'000'000.0;

		return ( current - started ) * 1000.0;
	#else
		auto* file = std::fopen( "/proc/self/stat", "r" );

		if ( file == nullptr ) {
			return -1.0;
		}

		char buffer[ 4096 ]{ };

		if ( std::fgets( buffer, sizeof( buffer ), file ) == nullptr ) {
			std::fclose( file );

			return -1.0;
		}

		std::fclose( file );

		// The comm field is parenthesised and may contain spaces, so fields are counted from
		// the last ')' — starttime is field 22, the 20th after the state field.
		auto* close = std::strrchr( buffer, ')' );

		if ( close == nullptr ) {
			return -1.0;
		}

		unsigned long long start_ticks = 0;
		auto fields_after_comm = 0;
		auto* cursor = close + 1;

		while ( *cursor != '\0' && fields_after_comm < 20 ) {
			while ( *cursor == ' ' ) {
				++cursor;
			}

			if ( *cursor == '\0' ) {
				break;
			}

			if ( fields_after_comm == 19 ) {
				start_ticks = std::strtoull( cursor, nullptr, 10 );

				break;
			}

			while ( *cursor != '\0' && *cursor != ' ' ) {
				++cursor;
			}

			++fields_after_comm;
		}

		const auto ticks_per_second = static_cast< double >( ::sysconf( _SC_CLK_TCK ) );
		const auto uptime_seconds = static_cast< double >( start_ticks ) / ticks_per_second;

		auto* uptime_file = std::fopen( "/proc/uptime", "r" );

		if ( uptime_file == nullptr ) {
			return -1.0;
		}

		double system_uptime = 0.0;

		if ( std::fscanf( uptime_file, "%lf", &system_uptime ) != 1 ) {
			std::fclose( uptime_file );

			return -1.0;
		}

		std::fclose( uptime_file );

		return ( system_uptime - uptime_seconds ) * 1000.0;
	#endif
	}

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

		// statm reports PAGES. Assuming 4 KiB understates every RSS number by the
		// page ratio on a 16 KiB-page kernel, which is the common arm64 default.
		return resident * static_cast< unsigned long long >( page_size_bytes( ) ) / 1024;
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
		const auto elapsed = std::chrono::duration_cast< std::chrono::nanoseconds >(
			clock_type::now( ) - start ).count( );

		return static_cast< double >( elapsed ) / 1'000'000.0;
	}

	// The full frozen surface, registered with stubs. Measuring a
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

	// Distinct from load_failures: a measurement that could not be taken is not a
	// load failure, but it must still fail the run. Swallowing it would let the
	// gate pass on a run that produced no number at all.
	auto measurement_failures = 0;

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
			// Each of these skips a gated metric entirely. Reporting and counting
			// them is what lets the gate require every metric rather than tolerate
			// a missing one.
			std::printf( "  dispatch(%d): VM creation failed: %s\n", handler_count,
				host.error( ).msg.c_str( ) );
			++measurement_failures;

			continue;
		}

		if ( !register_api_surface( *host ) ) {
			std::printf( "  dispatch(%d): API registration failed\n", handler_count );
			++measurement_failures;

			continue;
		}

		auto dispatch_source = build_dispatch_source( handler_count );

		if ( !host->run( dispatch_source, "=(dispatch)" ) ) {
			std::printf( "  dispatch(%d): the dispatch source failed to run\n", handler_count );
			++measurement_failures;

			continue;
		}

		const auto started = clock_type::now( );
		auto completed = 0;

		for ( auto index = 0; index < dispatch_iterations; ++index ) {
			auto result = host->call_global( "dispatch", "x" );

			if ( !result ) {
				// Report it. Silently breaking here divided the elapsed time of a
				// handful of iterations by the full count, so a dispatch that failed
				// immediately measured as the fastest one in the run.
				std::printf( "  dispatch with %d handlers failed after %d iterations: %s\n",
					handler_count, index, result.error( ).msg.c_str( ) );
				++measurement_failures;

				break;
			}

			++completed;
		}

		if ( completed == 0 ) {
			continue;
		}

		// Divided by the iterations that actually ran, not the ones requested.
		const auto total_ms = milliseconds_since( started );
		dispatch_per_call_us.push_back( total_ms * 1000.0 / completed );

		std::printf( "dispatch_handlers_%d_us_per_call=%.4f\n", handler_count,
			dispatch_per_call_us.back( ) );
	}

	// --- fixed cost of a bare VM, no API surface, no extension code ---------
	// Isolates what `luaL_newstate` + `luaL_openlibs` + the sandbox costs, so a
	// per-extension budget can distinguish the fixed VM from the extension's own
	// allocations.
	{
		auto bare = mcode::lua_host::create( mcode::lua_host_options{ .extension_name = "bare", .module_loader = { } } );

		if ( !bare ) {
			std::printf( "  bare VM creation failed: %s\n", bare.error( ).msg.c_str( ) );
			++measurement_failures;
		}

		if ( bare ) {
			// Force sealing without running anything, which is what installs the
			// sandbox and the thread.
			auto sealed = bare->run( "return 0", "=(bare)" );

			if ( !sealed ) {
				std::printf( "  bare VM failed to seal: %s\n", sealed.error( ).msg.c_str( ) );
				++measurement_failures;
			}

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

		if ( !descriptor ) {
			// Previously silent. A declarative descriptor that fails to load emits no
			// timing line at all, so the gate would see a missing metric and -- before
			// the gate was fixed -- pass.
			std::printf( "provider_declarative_error=descriptor:%s\n",
				descriptor.error( ).msg.c_str( ) );
			++measurement_failures;
		}

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

			// The raw count depends on the iteration count, which is a command-line
			// argument, so it cannot be a fixed baseline. The invariant that matters
			// is one event per iteration -- a path that drops events is the failure
			// this catches, and it would otherwise look like a suspiciously fast
			// escape hatch.
			std::printf( "provider_declarative_events_per_iteration=%.6f\n",
				static_cast< double >( total ) / static_cast< double >( dispatch_iterations ) );
		}

		auto hatch = mcode::model::descriptor_from_json( hatch_json );

		if ( !hatch ) {
			std::printf( "provider_escape_hatch_error=descriptor:%s\n", hatch.error( ).msg.c_str( ) );
			++measurement_failures;
		}

		if ( hatch ) {
			// The hatch parses in Lua, using the VM's own string and table work --
			// the shape a real exotic provider would need. This is the cost the
			// declarative path exists to avoid.
			auto host = mcode::lua_host::create( mcode::lua_host_options{ .extension_name = "hatch", .module_loader = { } } );

			if ( !host ) {
				std::printf( "provider_escape_hatch_error=vm:%s\n", host.error( ).msg.c_str( ) );
				++measurement_failures;
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
					++measurement_failures;
				}

				if ( ran ) {
					auto applier = mcode::model::delta_applier{ *hatch };
					auto lua_host_pointer = &*host;

					// The name is unused because this probe drives one event shape; the
					// measured crossing is the payload marshalling either way.
					applier.set_escape_hatch( [lua_host_pointer]( const std::string_view,
						const std::string_view data ) -> mcode::result< std::vector< mcode::model::chat_event > > {
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
					std::printf( "provider_escape_hatch_events_per_iteration=%.6f\n",
						static_cast< double >( total ) /
							static_cast< double >( dispatch_iterations ) );
				}
			}
		}
	}

	// --- report -------------------------------------------------------------
	const auto rss_now = current_rss_kb( );

	std::printf( "extensions=%d\n", loaded );
	std::printf( "tools_per_extension=%d\n", tools_per_extension );
	std::printf( "load_failures=%d\n", load_failures );
	std::printf( "measurement_failures=%d\n", measurement_failures );
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

	// Cold start is measured from the OS, not from a timer in `main`, so it includes the
	// loader and C-runtime start-up the budget in `01` is actually about. A probe that could
	// not read the process creation time emits NO metric rather than a sentinel: the gate
	// treats a missing gated metric as a failure, which is what a broken probe is.
	const auto cold_start_ms = process_age_ms( );

	if ( cold_start_ms < 0.0 ) {
		++measurement_failures;
	} else {
		std::printf( "cold_start_ms=%.3f\n", cold_start_ms );

		if ( cold_start_ms > COLD_START_BUDGET_MS ) {
			std::printf( "cold_start_budget=FAIL exceeded %.1f ms\n", COLD_START_BUDGET_MS );
		} else {
			std::printf( "cold_start_budget=ok\n" );
		}
	}

	return ( load_failures != 0 || measurement_failures != 0 ) ? 1 : 0;
}
