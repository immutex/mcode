#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/eval/suite.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/net/sse.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"
#include "mcode/support/text.hxx"

namespace {

	int g_failures = 0;
	int g_checks = 0;

	auto check( const bool condition, const std::string_view label, const std::string_view detail = { } )
		-> void {
		++g_checks;

		if ( condition ) {
			std::printf( "  [ok]   %.*s\n", static_cast< int >( label.size( ) ), label.data( ) );

			return;
		}

		++g_failures;
		std::printf( "  [FAIL] %.*s", static_cast< int >( label.size( ) ), label.data( ) );

		if ( !detail.empty( ) ) {
			std::printf( " -- %.*s", static_cast< int >( detail.size( ) ), detail.data( ) );
		}

		std::printf( "\n" );
	}

	auto section( const std::string_view title ) -> void {
		std::printf( "\n== %.*s ==\n", static_cast< int >( title.size( ) ), title.data( ) );
	}

	auto build_core_registry( mcode::tool_registry& registry ) -> void {
		const struct {
			const char* name;
			const char* description;
			mcode::tool_class klass;
		} CORE_TOOLS[] = {
			{ "read", "Read a file window with line numbers", mcode::tool_class::read },
			{ "edit", "Replace an exact string in a file", mcode::tool_class::write },
			{ "write", "Create or overwrite a file", mcode::tool_class::write },
			{ "glob", "Find files by pattern", mcode::tool_class::read },
			{ "grep", "Search file contents by regex", mcode::tool_class::read },
			{ "bash", "Run a shell command", mcode::tool_class::exec },
			{ "ask_user", "Ask the user a question", mcode::tool_class::read },
			{ "tool_search", "Find and load deferred tool schemas", mcode::tool_class::read },
		};

		for ( const auto& tool : CORE_TOOLS ) {
			auto definition = mcode::tool_def{ };
			definition.name = tool.name;
			definition.description = tool.description;
			definition.klass = tool.klass;
			definition.source = mcode::tool_source::core;
			definition.deferrable = false;

			if ( auto added = registry.add( std::move( definition ) ); !added ) {
				std::printf( "  registry error: %s\n", added.error( ).msg.c_str( ) );
				++g_failures;
			}
		}
	}

}

namespace {

	// `mcode exec [options] [prompt]` -- the headless surface (docs/22 E4).
	// Returns the process exit code.
	auto run_exec( const std::vector< std::string >& arguments ) -> int {
		auto parsed = mcode::cli::parse_exec_options( arguments );

		if ( !parsed ) {
			std::fprintf( stderr, "mcode: %s\n\n", parsed.error( ).msg.c_str( ) );
			std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

			return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
		}

		// Unknown flags are refused, never ignored: a typo like `--max-step` would
		// otherwise run with the default budget and the user would never know.
		if ( !parsed->unknown_arguments.empty( ) ) {
			std::fprintf( stderr, "mcode: unknown argument '%s'\n\n",
				parsed->unknown_arguments.front( ).c_str( ) );
			std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

			return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
		}

		auto stream = mcode::cli::json_stream{ parsed->json };
		stream.emit_run_start( parsed->prompt );

		auto bus = mcode::events::bus{ };
		auto sequence = std::uint64_t{ 0 };

		// Mirror every event onto the JSON stream. This is the coupling docs/20
		// describes: the TUI, the session log, and the headless stream are all
		// subscribers, and none of them is special.
		bus.subscribe( mcode::events::kind::session_start,
			[&stream, &sequence]( const mcode::events::event& value ) {
				auto mirrored = value;
				mirrored.sequence = sequence++;
				stream.emit_event( mirrored );
			} );

		auto start = mcode::events::event{ };
		start.type = mcode::events::kind::session_start;
		start.payload_json = R"({"headless":true})";
		bus.publish( start );

		auto end = mcode::events::event{ };
		end.type = mcode::events::kind::session_end;
		end.payload_json = R"({"reason":"m0-skeleton"})";
		bus.publish( end );

		// M0 has no model client, so a run cannot yet produce a result. Saying so
		// with the provider-error code is honest; exiting 0 would claim a
		// verification that never ran.
		stream.emit_run_end( mcode::cli::exit_code::provider_error,
			"no model client in M0 (docs/16 M0)" );

		if ( parsed->verbose ) {
			std::fprintf( stderr, "mcode: exec finished (M0 skeleton, no model client)\n" );
		}

		return mcode::cli::to_int( mcode::cli::exit_code::provider_error );
	}

}

auto main( int argument_count, char** arguments ) -> int {
	auto argv = std::vector< std::string >{ };

	for ( auto index = 1; index < argument_count; ++index ) {
		argv.emplace_back( arguments[ index ] );
	}

	if ( !argv.empty( ) && argv.front( ) == "exec" ) {
		return run_exec( { argv.begin( ) + 1, argv.end( ) } );
	}

	// `mcode eval [--json] [fixture-root]` -- the deterministic suite (docs/26 E6/E7).
	// No model involved: every task asserts a harness behaviour, so a failure is
	// always a real regression rather than sampling noise.
	if ( !argv.empty( ) && argv.front( ) == "eval" ) {
		auto json = false;
		auto fixture = std::filesystem::path{ "tests/fixtures/fixture-repo" };

		for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
			if ( argv[ index ] == "--json" ) {
				json = true;
			} else if ( argv[ index ] == "--fixtures" && index + 1 < argv.size( ) ) {
				fixture = argv[ ++index ];
			} else {
				std::fprintf( stderr, "mcode: unknown eval argument '%s'\n",
					argv[ index ].c_str( ) );

				return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
			}
		}

		if ( !std::filesystem::exists( fixture ) ) {
			std::fprintf( stderr, "mcode: fixture repo not found: %s\n", fixture.string( ).c_str( ) );

			return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
		}

		const auto result = mcode::eval::run_suite( fixture );

		if ( json ) {
			std::fputs( mcode::eval::to_jsonl( result ).c_str( ), stdout );
		} else {
			for ( const auto& record : result.records ) {
				std::printf( "  [%-5s] %s\n", record.verdict.c_str( ), record.task_id.c_str( ) );

				if ( !record.detail.empty( ) ) {
					std::printf( "          %s\n", record.detail.c_str( ) );
				}
			}

			std::printf( "\n  %zu passed, %zu failed, %zu errored (of %zu)\n", result.passed,
				result.failed, result.errored, result.total( ) );
		}

		return result.all_passed( ) ? mcode::cli::to_int( mcode::cli::exit_code::success )
			: mcode::cli::to_int( mcode::cli::exit_code::verification_failed );
	}

	if ( !argv.empty( ) && ( argv.front( ) == "--help" || argv.front( ) == "-h" ) ) {
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stdout );

		return 0;
	}

	if ( !argv.empty( ) ) {
		std::fprintf( stderr, "mcode: unknown command '%s'\n\n", argv.front( ).c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	std::printf( "mcode %.*s -- startup smoke test\n", static_cast< int >( mcode::VERSION.size( ) ),
		mcode::VERSION.data( ) );
	std::printf( "platform: %.*s | compiler: %.*s | build: %.*s\n",
		static_cast< int >( mcode::PLATFORM.size( ) ), mcode::PLATFORM.data( ),
		static_cast< int >( mcode::COMPILER.size( ) ), mcode::COMPILER.data( ),
		static_cast< int >( mcode::BUILD_TYPE.size( ) ), mcode::BUILD_TYPE.data( ) );

	section( "spdlog (logging)" );
	mcode::init_logging( mcode::log_level::info );
	check( mcode::logging_initialized( ), "async logger initialised" );

	if ( auto log = mcode::logger( ) ) {
		log->info( "mcode smoke test starting" );
	}

	check( true, "log call did not throw" );

	section( "C++23 library support" );
	const auto support = mcode::detect_library_support( );
	std::printf( "  generator=%d move_only_function=%d print=%d expected=%d\n", support.generator ? 1 : 0,
		support.move_only_function ? 1 : 0, support.print ? 1 : 0, support.expected ? 1 : 0 );
	check( support.expected, "std::expected available" );

	section( "yyjson (JSON)" );

	{
		auto doc = mcode::json::document::parse( R"({"model":"mcode","steps":3,"nested":{"a":1}})" );
		check( static_cast< bool >( doc ), "parsed a JSON object" );

		if ( doc ) {
			auto model = doc->get_string( "model" );
			check( model && *model == "mcode", "read a string member" );

			auto steps = doc->get_int( "steps" );
			check( steps && *steps == 3, "read an integer member" );

			auto nested = doc->pointer( "/nested/a" );
			check( static_cast< bool >( nested ), "resolved a JSON Pointer" );

			auto missing = doc->get_int( "nope" );
			check( !missing, "missing key reports an error rather than defaulting" );

			auto serialised = doc->dump( false );
			check( static_cast< bool >( serialised ), "serialised a parsed document" );
		}

		auto malformed = mcode::json::document::parse( "{not json" );
		check( !malformed, "malformed JSON reports an error with a byte offset" );

		auto builder = mcode::json::document::make_object( );
		const auto accepted = static_cast< bool >( builder.set_string( "name", "mcode" ) ) &&
			static_cast< bool >( builder.set_int( "version", 1 ) );
		check( accepted, "mutable DOM accepted string and integer members" );

		auto built = builder.dump( false );
		check( built && built->find( "\"name\"" ) != std::string::npos, "mutable DOM built and serialised" );
	}

	section( "simdutf (Unicode)" );

	{
		const auto ascii = std::string{ "hello" };
		const auto utf8 = std::string{ "\xE4\xB8\x96\xE7\x95\x8C" };
		check( mcode::text::is_valid_utf8( ascii ), "validated ASCII" );
		check( mcode::text::is_valid_utf8( utf8 ), "validated multi-byte UTF-8" );
		check( !mcode::text::is_valid_utf8( "\xFF\xFE" ), "rejected invalid UTF-8" );
		check( mcode::text::codepoint_count( utf8 ) == 2, "counted code points, not bytes" );

		const auto cut = mcode::text::truncate( utf8, 4 );
		check( mcode::text::is_valid_utf8( cut ), "truncation landed on a code-point boundary" );

		const auto cleaned = mcode::text::sanitize_utf8( "ok\xFF\xFEok" );
		check( mcode::text::is_valid_utf8( cleaned ), "sanitised an invalid buffer to valid UTF-8" );

		auto wide = mcode::text::to_utf16( utf8 );
		check( wide && wide->size( ) == 2, "transcoded UTF-8 to UTF-16 for Win32 APIs" );

		if ( wide ) {
			auto back = mcode::text::from_utf16( *wide );
			check( back && *back == utf8, "round-tripped UTF-16 back to UTF-8" );
		}

		std::printf( "  prefer_wide_paths() = %s\n",
			mcode::text::prefer_wide_paths( ) ? "true" : "false" );
	}

	section( "ankerl::unordered_dense (tool registry)" );

	{
		auto registry = mcode::tool_registry{ };
		build_core_registry( registry );
		check( registry.size( ) == 8, "registered exactly the 8 core tools" );

		check( registry.find( "read" ) != nullptr, "found a tool by name" );
		check( registry.find( "nope" ) == nullptr, "absent tool returns nullptr" );

		auto duplicate = mcode::tool_def{ };
		duplicate.name = "read";
		duplicate.source = mcode::tool_source::core;
		check( !registry.add( std::move( duplicate ) ), "duplicate tool name is rejected" );

		const auto all = registry.all( );
		auto sorted = true;

		for ( auto index = std::size_t{ 1 }; index < all.size( ); ++index ) {
			if ( all[ index - 1 ]->name > all[ index ]->name ) {
				sorted = false;
			}
		}

		check( sorted, "all() is sorted by name" );
	}

	section( "Luau (extension layer)" );

	{
		auto host = mcode::lua_host::create( mcode::lua_host_options{ .extension_name = "smoke", .module_loader = { } } );
		check( static_cast< bool >( host ), "created a lua_State" );

		if ( host ) {
			std::printf( "  version: %s\n", host->version_string( ).c_str( ) );

			auto registered = host->register_host_function(
				"host_ping", []( const std::string_view args ) -> mcode::result< std::string > {
					return std::string{ "pong:" } + std::string{ args };
				} );
			check( static_cast< bool >( registered ), "registered a host function into mcode.*" );

			auto pinged = host->eval_to_string( "mcode.host_ping('hello')" );
			check( pinged && *pinged == "pong:hello", "Luau called back into C++" );

			auto ran = host->run( "x = 1 + 1" );
			check( static_cast< bool >( ran ), "executed a Luau chunk" );

			auto value = host->eval_to_string( "x * 21" );
			check( value && *value == "42", "evaluated Luau and read the result" );

			// Globals the extension sets land in its own table, not the host's.
			check( host->sealed( ), "the API surface was sealed before extension code ran" );

			auto late = host->register_host_function( "too_late", []( const std::string_view )
				-> mcode::result< std::string > { return std::string{ }; } );
			check( !late, "registration after sealing is refused" );

			auto broken = host->run( "this is not luau" );
			check( !broken, "compile error returned as a value" );

			auto threw = host->run( "error('boom')" );
			check( !threw, "runtime error returned as a value" );

			section( "Luau boundary (every escape must fail)" );

			// Each probe is an expression that evaluates to "true" when the escape
			// SUCCEEDED. A probe that cannot run is a failure of the probe, not a
			// pass for the boundary.
			const struct {
				const char* expression;
				const char* label;
			} ESCAPES[] = {
				{ "(io ~= nil)", "io is absent" },
				{ "(package ~= nil)", "package is absent" },
				{ "(type(os.execute) ~= 'nil')", "os.execute is absent" },
				{ "(type(os.getenv) ~= 'nil')", "os.getenv is absent" },
				{ "(type(os.remove) ~= 'nil')", "os.remove is absent" },
				{ "(type(os.exit) ~= 'nil')", "os.exit is absent" },
				{ "(type(loadstring) ~= 'nil')", "loadstring is absent" },
				{ "(type(load) ~= 'nil')", "load is absent" },
				{ "(type(dofile) ~= 'nil')", "dofile is absent" },
				{ "(type(loadfile) ~= 'nil')", "loadfile is absent" },
				{ "(type(string.dump) ~= 'nil')", "string.dump is absent" },
				{ "(type(debug.getregistry) ~= 'nil')", "debug.getregistry is absent" },
				{ "(type(debug.setmetatable) ~= 'nil')", "debug.setmetatable is absent" },
				{ "(type(debug.sethook) ~= 'nil')", "debug.sethook is absent" },
				{ "(type(debug.getupvalue) ~= 'nil')", "debug.getupvalue is absent" },
				{ "(type(collectgarbage) ~= 'nil')", "collectgarbage is absent" },
				{ "select(1, pcall(rawset, _G, 'injected', 1))", "rawset on _G is refused" },
				{ "select(1, pcall(setmetatable, _G, {}))", "setmetatable on _G is refused" },
				{ "select(1, pcall(rawset, string, 'injected', 1))", "rawset on a library table is refused" },
				{ "select(1, pcall(rawset, mcode, 'injected', 1))", "rawset on the host API table is refused" },
				{ "select(1, pcall(setmetatable, '', { __index = function() return 1 end }))",
					"the string metatable is readonly" },
				{ "(type(mcode.not_registered) ~= 'nil')", "unregistered host functions are absent" },
				{ "(function() local host = getmetatable(getfenv(0)).__index "
				  "return tostring(select(1, pcall(rawset, host, 'injected', 1))) end)()",
					"the frozen host globals stay readonly when reached through getfenv" },
				{ "(function() setfenv(1, setmetatable({}, { __index = _G })) "
				  "return tostring(select(1, pcall(rawset, _G, 'injected', 1))) end)()",
					"replacing the environment with setfenv does not unfreeze _G" },
			};

			for ( const auto& probe : ESCAPES ) {
				auto outcome = host->eval_to_string( probe.expression );

				if ( !outcome ) {
					check( false, probe.label, "probe could not run: " + outcome.error( ).msg );

					continue;
				}

				check( *outcome == "false", probe.label, "escape succeeded: " + *outcome );
			}

			// Luau keeps these three from Lua 5.1. They are not escalations, so
			// the boundary is unaffected -- but a doc that claims the base library
			// is stripped would be wrong, so the smoke test records their presence.
			// Luau retains these Lua 5.1 shims. They are not escalations, but a doc
			// claiming the base library is stripped would be wrong, so their presence
			// is recorded here rather than assumed.
			const struct {
				const char* name;
				const char* label;
			} SHIMS[] = {
				{ "newproxy", "newproxy is present (creates a tagged userdata with no host reach)" },
				{ "setfenv", "setfenv is present (changes only the caller's own environment)" },
				{ "getfenv", "getfenv is present (returns the extension's own globals proxy)" },
				{ "require", "require is present (host-injected, extension-root only)" },
			};

			for ( const auto& shim : SHIMS ) {
				auto kind = host->eval_to_string( "tostring(type(" + std::string{ shim.name } + "))" );

				check( kind && *kind == "function", shim.label,
					kind ? *kind : std::string{ "probe failed" } );
			}

			// require is a capability, not an absence: it must refuse anything the host
			// did not hand it, and it must never touch the filesystem itself.
			auto require_refuses = host->eval_to_string(
				"tostring(select(1, pcall(require, '../../../etc/passwd')))" );
			check( require_refuses && *require_refuses == "false",
				"require refuses a path outside the extension root",
				require_refuses ? *require_refuses : std::string{ "probe failed" } );

			auto require_unknown = host->eval_to_string(
				"tostring(select(1, pcall(require, './lib/not_registered')))" );
			check( require_unknown && *require_unknown == "false",
				"require refuses a module the host never registered",
				require_unknown ? *require_unknown : std::string{ "probe failed" } );

			auto own_globals = host->eval_to_string(
				"(function() local ok = pcall(rawset, getfenv(0), 'own_global', 1) "
				"return tostring(ok and type(getfenv(0).own_global) == 'number') end)()" );
			check( own_globals && *own_globals == "true",
				"getfenv(0) is the extension's own writable namespace, not the host's",
				own_globals ? *own_globals : std::string{ "probe failed" } );

			auto proxy_safe = host->eval_to_string(
				"tostring(select(1, pcall(function() local p = newproxy(true) return p end)))" );
			check( proxy_safe && *proxy_safe == "true", "newproxy cannot be used to escape",
				proxy_safe ? *proxy_safe : std::string{ "probe failed" } );
		}
	}

	section( "Beast (SSE line parser)" );

	{
		auto events = std::vector< mcode::net::sse_event >{ };
		auto parser = mcode::net::sse_parser{
			[&]( mcode::net::sse_event&& event ) { events.push_back( std::move( event ) ); } };

		parser.feed( "event: message\ndata: {\"delta\":\"he" );
		parser.feed( "llo\"}\n\n" );
		parser.feed( ": keep-alive\n\n" );
		parser.feed( "data: {\"delta\":\" world\"}\n\n" );
		parser.finish( );

		check( events.size( ) == 2, "parsed exactly 2 events across 4 chunk boundaries" );

		if ( events.size( ) == 2 ) {
			auto first = mcode::net::extract_delta_text( events[ 0 ].data );
			auto second = mcode::net::extract_delta_text( events[ 1 ].data );
			check( first && *first == "hello", "extracted the first delta" );
			check( second && *second == " world", "extracted the second delta" );
		}

		auto done = mcode::net::extract_delta_text( "[DONE]" );
		check( done && done->empty( ), "[DONE] sentinel is not an error" );

		auto good = mcode::net::parse_url( "http://example.com:8080/v1/chat?x=1" );
		check( good && good->host == "example.com" && good->port == "8080", "parsed a normal URL" );

		auto userinfo = mcode::net::parse_url( "https://example.com@127.0.0.1/" );
		check( !userinfo, "rejected userinfo rather than mis-reading the host" );

		auto secure = mcode::net::parse_url( "https://api.example.com/v1" );
		check( secure && secure->port == "443", "defaulted the HTTPS port" );
	}

	section( "Boost.Process v2 (subprocess)" );

	{
		auto options = mcode::process_options{ };
		options.timeout = std::chrono::milliseconds{ 15'000 };

	#if defined( _WIN32 )
		options.executable = "cmd.exe";
		options.args = { "/c", "echo mcode-subprocess-ok" };
	#else
		options.executable = "/bin/sh";
		options.args = { "-c", "echo mcode-subprocess-ok" };
	#endif

		auto found = mcode::find_executable( options.executable );
		check( static_cast< bool >( found ), "resolved the executable on PATH" );

		if ( found ) {
			options.executable = *found;
		}

		auto result = mcode::run_process( options );
		check( static_cast< bool >( result ), "spawned and reaped a child process" );

		if ( result ) {
			check( result->exit_code == 0, "child exited 0" );
			check( result->stdout_text.find( "mcode-subprocess-ok" ) != std::string::npos,
				"captured stdout concurrently with stderr" );
			check( result->stderr_text.empty( ), "stderr was empty" );
			check( !result->timed_out, "did not hit the timeout" );
		}

		const auto environment = mcode::minimal_environment( );
		auto leaked = false;

		for ( const auto& [ key, value ] : environment ) {
			( void )value;

			if ( key.find( "TOKEN" ) != std::string::npos || key.find( "SECRET" ) != std::string::npos ||
				key.find( "KEY" ) != std::string::npos ) {
				leaked = true;
			}
		}

		check( !leaked, "minimal environment carries no credential-shaped variables" );
	}

	section( "filesystem (workspace boundary)" );

	{
		const auto current = std::filesystem::current_path( );
		auto opened = mcode::workspace::open( current );
		check( static_cast< bool >( opened ), "opened the current directory as a workspace" );

		if ( opened ) {
			auto inside = opened->resolve( "CMakeLists.txt" );
			check( static_cast< bool >( inside ), "resolved a path inside the workspace" );

			auto outside = opened->resolve( "../../../../../../etc/passwd" );
			check( !outside, "refused a path escaping the workspace" );

			check( !opened->contains( current.parent_path( ) / "mcode-evil" ),
				"component-wise containment rejects a prefix-sharing sibling" );
		}
	}

	section( "agent loop (budget + event log + dispatch)" );

	{
		auto registry = mcode::tool_registry{ };
		build_core_registry( registry );

		auto log = mcode::event_log{ };
		auto budget = mcode::session_budget{ };
		auto loop = mcode::agent_loop{ registry, log, budget };

		loop.register_handler( "read", []( const std::string_view args ) -> mcode::result< std::string > {
			return std::string{ "<file contents for " } + std::string{ args } + ">";
		} );

		auto ok = loop.execute( { "read", R"({"path":"README.md"})" } );
		check( ok.ok, "dispatched a registered tool" );
		check( log.size( ) == 2, "logged both the call and the result" );

		auto unknown = loop.execute( { "nope", "{}" } );
		check( !unknown.ok && unknown.code == mcode::errc::tool_failed,
			"unknown tool returned a value-level failure" );

		loop.budget( ).max_steps = loop.budget( ).steps_used;
		auto blocked = loop.execute( { "read", "{}" } );
		check( !blocked.ok && blocked.code == mcode::errc::cancelled, "budget exhaustion stops dispatch" );

		const auto jsonl = log.to_jsonl( );
		check( jsonl.find( "\"seq\":0" ) != std::string::npos, "event seq starts at 0" );
		check( jsonl.find( "\"v\":1" ) != std::string::npos, "event envelope carries v=1" );
	}

	section( "cli (headless surface)" );

	{
		// Exit codes are an interface a script branches on, so each one is asserted.
		check( mcode::cli::to_int( mcode::cli::exit_code::success ) == 0, "exit 0 is success" );
		check( mcode::cli::to_int( mcode::cli::exit_code::usage_error ) == 2,
			"exit 2 is a usage error" );
		check( mcode::cli::to_int( mcode::cli::exit_code::interrupted ) == 130,
			"exit 130 is interrupted" );

		// Unknown flags are collected, not ignored -- and they do NOT consume the
		// following token, because a flag we do not recognise has no arity. So in
		// this line "5" is the prompt, not the value of the typo.
		auto parsed = mcode::cli::parse_exec_options(
			{ "--json", "--model", "m", "do the thing", "--max-step" } );

		check( static_cast< bool >( parsed ), "parsed a valid command line" );

		if ( parsed ) {
			check( parsed->json, "--json was recognised" );
			check( parsed->model == "m", "--model took its value" );
			check( parsed->prompt == "do the thing", "the bare argument became the prompt" );
			check( parsed->unknown_arguments.size( ) == 1,
				"the typo --max-step was collected rather than ignored" );
			check( parsed->unknown_arguments.front( ) == "--max-step",
				"the collected argument is the typo itself" );
		}

		// An unknown flag does not swallow the next token: a second bare argument is
		// also collected, so nothing is silently dropped.
		auto arity = mcode::cli::parse_exec_options( { "--max-step", "5", "prompt" } );

		check( static_cast< bool >( arity ), "parsed with an unknown flag present" );

		if ( arity ) {
			check( arity->prompt == "5", "the unknown flag did not consume its neighbour" );
			check( arity->unknown_arguments.size( ) == 2,
				"both the typo and the extra bare argument were collected" );
		}

		// A flag missing its value is an error, not an empty string.
		auto missing = mcode::cli::parse_exec_options( { "--model" } );
		check( !missing, "a flag without a value is refused" );

		auto bad_number = mcode::cli::parse_exec_options( { "--max-steps", "lots" } );
		check( !bad_number, "a non-numeric flag value is refused" );

		// The JSON stream pairs run.start with exactly one run.end.
		auto stream = mcode::cli::json_stream{ true };
		stream.emit_run_start( "prompt" );

		auto event = mcode::events::event{ };
		event.type = mcode::events::kind::tool_call;
		event.payload_json = R"({"name":"read"})";
		stream.emit_event( event );

		stream.emit_run_end( mcode::cli::exit_code::success, "done" );
		stream.emit_run_end( mcode::cli::exit_code::provider_error, "second" );

		check( stream.run_end_emitted( ), "run.end was emitted" );
		check( stream.lines_emitted( ) == 3, "run.start, one event, and exactly one run.end" );
	}

	section( "extension loader (docs/26 E8)" );

	{
		// The real path: discover, validate, install the frozen API, run init.luau,
		// and call the registered tool back. No stubs.
		auto registry = mcode::tool_registry{ };
		auto providers = mcode::model::provider_registry{ };
		auto bus = mcode::events::bus{ };
		auto hooks = mcode::ext::hook_registry{ bus };

		const auto extensions = std::filesystem::path{ MCODE_SMOKE_EXTENSIONS };

		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( registry );

		auto loaded = mcode::ext::load_extensions( { extensions }, providers, hooks, options );

		for ( const auto& failure : loaded.report.failed ) {
			std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
				failure.reason.c_str( ) );
		}

		// The fixture root deliberately holds one good extension and one whose
		// manifest has a typo. Both outcomes are the point: the good one loads, the
		// bad one fails, and the failure does not take the session with it.
		check( loaded.report.loaded.size( ) == 1, "the valid fixture extension loaded" );
		check( loaded.report.failed.size( ) == 1, "the invalid fixture extension failed" );
		check( loaded.report.loaded.front( ).name == "hello-tool",
			"the loaded extension is the valid one" );

		// A tool an extension registered is indistinguishable from a core tool to
		// the registry, except for its source and owner.
		const auto* hello = registry.find( "hello" );
		check( hello != nullptr, "the extension's tool is in the registry" );

		if ( hello != nullptr ) {
			check( hello->owner == "hello-tool", "the tool is attributed to its extension" );
			check( !hello->is_core( ), "the tool is not core" );
		}

		// And it runs: C++ registry -> VM closure -> extension code -> back.
		auto greeting = loaded.invoke( "hello", R"({"name":"smoke"})" );
		check( greeting && greeting->find( "hello from hello-tool to smoke" ) != std::string::npos,
			"the tool ran and returned the extension's answer",
			greeting ? *greeting : greeting.error( ).msg );

		// An environmental failure comes back as a message, not an empty success.
		auto refused = loaded.invoke( "hello", "{}" );
		check( !refused && refused.error( ).msg == "name is required",
			"an environmental failure returns the extension's message",
			refused ? std::string{ "unexpectedly succeeded" } : refused.error( ).msg );

		section( "extension hooks (a hook written in Luau)" );

		// docs/26's exit criterion names a provider, a tool, AND a hook. The hook is
		// registered by init.luau, dispatched by the bus, and its veto is attributed.
		check( hooks.handlers_for( "tool.pre_call" ) == 1, "the extension's hook is subscribed" );
		check( hooks.handlers_for( "hello-tool.ready" ) == 1, "its custom event is subscribed" );

		auto allowed = mcode::events::event{ };
		allowed.type = mcode::events::kind::tool_pre_call;
		allowed.payload_json = R"({"name":"read"})";
		check( !bus.publish( allowed ), "an unobjectionable tool call passes the hook" );

		auto blocked = mcode::events::event{ };
		blocked.type = mcode::events::kind::tool_pre_call;
		blocked.payload_json = R"({"name":"forbidden"})";

		auto veto = bus.publish( blocked );
		check( veto && veto->reason == "blocked by hello-tool" && veto->source == "hello-tool",
			"the hook vetoed the call, attributed to the extension",
			veto ? veto->reason : std::string{ "no veto" } );

		// A custom event dispatches inside the hook registry and never becomes a
		// session kind: docs/20 keeps the log's schema closed.
		if ( !loaded.extensions.empty( ) && loaded.extensions.front( ).host != nullptr ) {
			check( static_cast< bool >( hooks.emit( *loaded.extensions.front( ).host,
				"hello-tool.ready", R"({"count":7})" ) ),
				"a custom event reaches the extension" );
		}

		check( hooks.total_failures( ) == 0, "no hook threw" );

		section( "extension loader (disable all)" );

		// docs/23's mechanical check: with every extension disabled the agent must
		// still have its core tools and must not error. This is what proves the
		// core/extension line has not drifted.
		auto bare_registry = mcode::tool_registry{ };

		for ( const auto* name : { "read", "write", "edit", "glob", "grep", "bash" } ) {
			auto core = mcode::tool_def{ };
			core.name = name;
			core.source = mcode::tool_source::core;

			if ( !bare_registry.add( std::move( core ) ) ) {
				check( false, "could not build the core registry" );

				break;
			}
		}

		auto bare_providers = mcode::model::provider_registry{ };
		auto bare_bus = mcode::events::bus{ };
		auto bare_hooks = mcode::ext::hook_registry{ bare_bus };

		auto disabled_options = mcode::ext::loader_options{ };
		disabled_options.disabled = true;
		disabled_options.register_api = mcode::ext::default_register_api( bare_registry );

		auto bare = mcode::ext::load_extensions( { extensions }, bare_providers, bare_hooks,
			disabled_options );

		check( bare.report.loaded.empty( ), "nothing loaded when disabled" );
		check( bare.report.failed.empty( ), "disabling is not a failure" );
		check( bare.report.disabled == 2, "both extensions were counted as disabled" );
		check( bare_registry.size( ) == 6, "the core tools survived" );
		check( bare_registry.find( "read" ) != nullptr, "a core tool is still present" );
		check( bare_registry.find( "hello" ) == nullptr, "the extension tool is absent" );
		check( bare_providers.empty( ), "no provider was declared" );

		// Invoking what would have been there is a clean error, not a crash.
		auto absent = bare.invoke( "hello", "{}" );
		check( !absent, "invoking a disabled tool is an error" );

		section( "extension loader (shipped extensions)" );

		// The extensions the product ships. `providers` declares three model
		// providers through `mcode.model.register`, which is the D3 dogfood claim
		// exercised through the real loader rather than a hand-written mirror.
		auto shipped_registry = mcode::tool_registry{ };
		auto shipped_providers = mcode::model::provider_registry{ };
		auto shipped_bus = mcode::events::bus{ };
		auto shipped_hooks = mcode::ext::hook_registry{ shipped_bus };

		auto shipped_options = mcode::ext::loader_options{ };
		shipped_options.register_api = mcode::ext::default_register_api( shipped_registry );

		auto shipped = mcode::ext::load_extensions(
			{ std::filesystem::path{ MCODE_SMOKE_SHIPPED_EXTENSIONS } }, shipped_providers,
			shipped_hooks, shipped_options );

		for ( const auto& failure : shipped.report.failed ) {
			std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
				failure.reason.c_str( ) );
		}

		check( shipped.report.loaded.size( ) == 1, "the shipped extensions directory is not empty" );
		check( shipped.report.failed.empty( ), "every shipped extension loaded" );
		check( shipped_providers.size( ) == 3, "the providers extension declared three providers" );
		check( shipped_providers.find( "openai-chat-completions" ) != nullptr,
			"the OpenAI descriptor is registered" );

		if ( const auto* descriptor = shipped_providers.find( "anthropic-messages" );
			descriptor != nullptr ) {
			check( descriptor->endpoint.starts_with( "https://" ),
				"the descriptor carries a real endpoint" );
		}
	}

	section( "summary" );
	std::printf( "  %d checks, %d failures\n", g_checks, g_failures );

	if ( auto log = mcode::logger( ) ) {
		log->info( "smoke test finished: {} checks, {} failures", g_checks, g_failures );
	}

	mcode::shutdown_logging( );

	( void )argument_count;

	return g_failures;
}
