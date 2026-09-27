#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
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

auto main( int argument_count, char** arguments ) -> int {
	( void )arguments;

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

	section( "LuaJIT (extension layer)" );

	{
		auto host = mcode::lua_host::create( { .extension_name = "smoke" } );
		check( static_cast< bool >( host ), "created a lua_State" );

		if ( host ) {
			check( !host->jit_enabled( ), "JIT is off by default" );
			std::printf( "  %s\n", host->version_string( ).c_str( ) );

			auto ran = host->run( "x = 1 + 1" );
			check( static_cast< bool >( ran ), "executed a Lua chunk" );

			auto value = host->eval_to_string( "x * 21" );
			check( value && *value == "42", "evaluated Lua and read the result" );

			auto broken = host->run( "this is not lua" );
			check( !broken, "compile error returned as a value" );

			auto threw = host->run( "error('boom')" );
			check( !threw, "runtime error returned as a value" );

			auto registered = host->register_host_function(
				"host_ping", []( const std::string_view args ) -> mcode::result< std::string > {
					return std::string{ "pong:" } + std::string{ args };
				} );
			check( static_cast< bool >( registered ), "registered a host function into mcode.*" );

			auto pinged = host->eval_to_string( "mcode.host_ping('hello')" );
			check( pinged && *pinged == "pong:hello", "Lua called back into C++" );

			auto ffi_present = host->eval_to_string( "type(require('ffi'))" );
			check( ffi_present && *ffi_present == "table", "ffi is available, so the VM is not a sandbox" );
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

	section( "summary" );
	std::printf( "  %d checks, %d failures\n", g_checks, g_failures );

	if ( auto log = mcode::logger( ) ) {
		log->info( "smoke test finished: {} checks, {} failures", g_checks, g_failures );
	}

	mcode::shutdown_logging( );

	( void )argument_count;

	return g_failures;
}
