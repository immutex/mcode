#include <array>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli_session.hxx"
#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/cli/skill_command.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/eval/suite.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/net/sse.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"
#include "mcode/support/text.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/perm/approval_headless.hxx"
#include "mcode/perm/approval_terminal.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/tools/register.hxx"
#include "smoke.hxx"

using smoke::check;
using smoke::g_checks;
using smoke::g_failures;
using smoke::section;

namespace {

	std::vector< std::pair< std::string,
		std::function< mcode::result< std::string >( std::string_view ) > > > pending_handlers;

	auto build_core_registry( mcode::tool_registry& registry, mcode::tools::tool_context& context )
		-> mcode::status {
		auto sink = mcode::tools::vector_sink{ };

		auto registered = mcode::tools::register_core_tools( registry, sink, context );

		if ( !registered ) {
			return registered;
		}

		pending_handlers = sink.take( );

		return { };
	}

}

auto run_smoke( ) -> int;

auto main( int argument_count, char** arguments ) -> int {
	auto argv = std::vector< std::string >{ };

	for ( auto index = 1; index < argument_count; ++index ) {
		argv.emplace_back( arguments[ index ] );
	}

	if ( !argv.empty( ) && argv.front( ) == "exec" ) {
		return run_exec( { argv.begin( ) + 1, argv.end( ) } );
	}

	if ( !argv.empty( ) && argv.front( ) == "skill" ) {
		return mcode::cli::run_skill( { argv.begin( ) + 1, argv.end( ) } );
	}

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
			std::fprintf( stderr, "mcode: fixture repo not found: %s\n",
				fixture.string( ).c_str( ) );

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

	if ( !argv.empty( ) && ( argv.front( ) == "--version" || argv.front( ) == "-V" ) ) {
		std::printf( "%s %s (%s, %s)\n", std::string{ mcode::NAME }.c_str( ),
			std::string{ mcode::VERSION }.c_str( ), std::string{ mcode::PLATFORM }.c_str( ),
			std::string{ mcode::COMPILER }.c_str( ) );

		return 0;
	}

	if ( !argv.empty( ) && argv.front( ) == "--smoke" ) {
		return run_smoke( );
	}

	// A leading flag belongs to the interactive session; only a bare word is unknown.
	if ( !argv.empty( ) && !argv.front( ).empty( ) && argv.front( ).front( ) != '-' ) {
		std::fprintf( stderr, "mcode: unknown command '%s'\n\n", argv.front( ).c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	return run_repl( argv );
}

auto run_smoke( ) -> int {
	std::printf( "mcode %.*s -- startup smoke test\n", static_cast< int >( mcode::VERSION.size( ) ),
		mcode::VERSION.data( ) );
	std::printf( "platform: %.*s | compiler: %.*s | build: %.*s\n",
		static_cast< int >( mcode::PLATFORM.size( ) ), mcode::PLATFORM.data( ),
		static_cast< int >( mcode::COMPILER.size( ) ), mcode::COMPILER.data( ),
		static_cast< int >( mcode::BUILD_TYPE.size( ) ), mcode::BUILD_TYPE.data( ) );

	section( "spdlog (logging)" );
	// No directory is requested, so this cannot fail here.
	check( static_cast< bool >( mcode::init_logging( mcode::log_level::info ) ),
		"async logger initialised" );
	check( mcode::logging_initialized( ), "async logger initialised" );

	if ( auto log = mcode::logger( ) ) {
		log->info( "mcode smoke test starting" );
	}

	check( true, "log call did not throw" );

	section( "C++23 library support" );
	const auto support = mcode::detect_library_support( );
	std::printf( "  generator=%d move_only_function=%d print=%d expected=%d\n",
		support.generator ? 1 : 0, support.move_only_function ? 1 : 0, support.print ? 1 : 0,
		support.expected ? 1 : 0 );
	check( support.expected, "std::expected available" );

	section( "yyjson (JSON)" );

	{
		auto doc = mcode::json::document::parse(
			R"({"model":"mcode","steps":3,"nested":{"a":1}})" );
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
		check( built && built->find( "\"name\"" ) != std::string::npos,
			"mutable DOM built and serialised" );
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
		check( mcode::text::is_valid_utf8( cleaned ),
			"sanitised an invalid buffer to valid UTF-8" );

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
		auto tools_context = mcode::tools::tool_context{ };
		auto registration = build_core_registry( registry, tools_context );
		check( static_cast< bool >( registration ), "registered the 8 core tools with schemas" );
		check( registry.size( ) == 8, "registered exactly the 8 core tools" );

		check( registry.find( "read" ) != nullptr, "found a tool by name" );
		check( registry.find( "nope" ) == nullptr, "absent tool returns nullptr" );

		auto schemas_valid = true;

		for ( const auto* definition : registry.all( ) ) {
			if ( definition->schema_json.empty( ) ||
				!static_cast< bool >( mcode::tools::validate_schema( definition->schema_json ) ) ) {
				schemas_valid = false;
			}
		}

		check( schemas_valid, "every core tool carries a valid schema" );

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

	smoke_luau( );

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

		for ( const auto& entry : environment ) {
			const auto& key = entry.first;

			if ( key.find( "TOKEN" ) != std::string::npos ||
				key.find( "SECRET" ) != std::string::npos ||
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

		auto space = mcode::workspace::open( std::filesystem::current_path( ) );
		check( static_cast< bool >( space ), "opened the workspace for tool dispatch" );

		if ( !space ) {
			::smoke_cli_and_extensions( );

			section( "summary" );
			std::printf( "  %d checks, %d failures\n", g_checks, g_failures );

			mcode::shutdown_logging( );

			return g_failures;
		}

		auto reads = mcode::tools::session_reads{ };

		auto store = mcode::perm::remember_store{
			std::filesystem::temp_directory_path( ) / "mcode-smoke-permissions.json" };
		auto engine = mcode::perm::permission_engine{ *space, &store };
		auto headless_source = mcode::perm::headless_approval_source{ };

		{
			auto engine_options = mcode::perm::permission_engine::options{ };
			engine_options.headless = true;
			engine.set_options( engine_options );
			engine.set_approval_source( &headless_source );
			std::ignore = engine.load_store( );
		}

		auto tools_context = mcode::tools::tool_context{ };
		tools_context.space = &*space;
		tools_context.reads = &reads;
		tools_context.permissions = &engine;
		tools_context.run_id = "smoke";
		tools_context.headless = true;

		auto registration = build_core_registry( registry, tools_context );
		check( static_cast< bool >( registration ), "registered the core tools for dispatch" );

		auto log = mcode::event_log{ };
		auto budget = mcode::session_budget{ };
		auto loop = mcode::agent_loop{ registry, log, budget };

		auto pending = pending_handlers.size( );

		for ( auto& [ name, handler ] : pending_handlers ) {
			loop.register_handler( name, handler );
		}

		check( pending == 8, "collected a handler for every core tool" );

		auto ok = loop.execute( { std::string{ }, "read", R"({"path":"README.md"})" } );
		check( ok.ok, "dispatched the real read tool" );
		check( ok.content.find( "coding-agent harness" ) != std::string::npos,
			"read returned actual file content, not a placeholder" );
		check( log.size( ) == 2, "logged both the call and the result" );

		auto unknown = loop.execute( { std::string{ }, "nope", "{}" } );
		check( !unknown.ok && unknown.code == mcode::errc::tool_failed,
			"unknown tool returned a value-level failure" );

		loop.budget( ).max_steps = loop.budget( ).steps_used;
		auto blocked = loop.execute( { std::string{ }, "read", "{}" } );
		check( !blocked.ok && blocked.code == mcode::errc::budget_exhausted,
			"budget exhaustion stops dispatch" );

		const auto jsonl = log.to_jsonl( );
		check( jsonl.find( "\"seq\":0" ) != std::string::npos, "event seq starts at 0" );
		check( jsonl.find( "\"v\":1" ) != std::string::npos, "event envelope carries v=1" );
	}

	::smoke_cli_and_extensions( );

	section( "summary" );
	std::printf( "  %d checks, %d failures\n", g_checks, g_failures );

	if ( auto log = mcode::logger( ) ) {
		log->info( "smoke test finished: {} checks, {} failures", g_checks, g_failures );
	}

	mcode::shutdown_logging( );

	return g_failures;
}
