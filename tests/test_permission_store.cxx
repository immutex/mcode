#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/perm/argv.hxx"

#include "permission_test_helpers.hxx"

namespace permission_test {

	TEST_CASE( "an allow-once answer does not persist; always does",
		"[perm][prompt][store]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );

		// allow_once is not remembered, so the same argv prompts again.
		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 2 );

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.exec( "npm install" ) == perm::permission_decision::allow );

		auto second_store = perm::remember_store{ setup.store.file( ) };
		auto second_engine = perm::permission_engine{ setup.space, &second_store };
		auto second_approval = scripted_source{ };
		second_engine.set_approval_source( &second_approval );

		const auto loaded = second_engine.load_store( );
		REQUIRE( loaded.has_value( ) );

		CHECK( second_approval.asks( ) == 0 );

		auto persisted = perm::permission_request{ };
		persisted.tool_name = "bash";
		persisted.klass = tool_class::exec;
		persisted.resource = "npm install";

		CHECK( second_engine.decide( persisted ) == perm::permission_decision::allow );
		CHECK( second_approval.asks( ) == 0 );
	}

	TEST_CASE( "a remembered allow is argv-exact", "[perm][store]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );

		setup.approval.queue( perm::approval_outcome::deny_once );
		CHECK( setup.exec( "git push" ) == perm::permission_decision::deny );

		setup.approval.queue( perm::approval_outcome::deny_once );
		CHECK( setup.exec( "git status --porcelain" ) == perm::permission_decision::deny );
	}

	TEST_CASE( "a remembered allow survives a fresh engine from the same file",
		"[perm][store]" ) {
		auto first = rig{ };
		first.approval.queue( perm::approval_outcome::allow_remember );
		REQUIRE( first.exec( "cmake --build build" ) == perm::permission_decision::allow );
		REQUIRE( first.store.file( ).string( ).find( "permissions.json" ) != std::string::npos );

		auto second = rig{ };
		auto second_store = perm::remember_store{ first.store.file( ) };
		auto second_engine = perm::permission_engine{ second.space, &second_store };
		second_engine.set_approval_source( &second.approval );

		REQUIRE( second_engine.load_store( ).has_value( ) );

		auto request = perm::permission_request{ };
		request.tool_name = "bash";
		request.klass = tool_class::exec;
		request.resource = "cmake --build build";

		CHECK( second_engine.decide( request ) == perm::permission_decision::allow );
		CHECK( second.approval.asks( ) == 0 );
	}

	TEST_CASE( "a project store cannot widen: allows are dropped with a warning",
		"[perm][store]" ) {
		// the store must sit under the workspace root to classify as project scope.
		auto setup = rig{ };
		auto file = setup.path / ".mcode" / "permissions.json";
		write_store_file( file,
			R"({"version":1,"exec":{"rm -rf /":"allow","npm test":"deny"}})" );

		auto project_store = perm::remember_store{ file };
		auto project_engine = perm::permission_engine{ setup.space, &project_store };
		project_engine.set_approval_source( &setup.approval );

		const auto loaded = project_engine.load_store( );
		REQUIRE( loaded.has_value( ) );

		// a project-scope allow is dropped (warned) while the deny survives.
		REQUIRE( project_engine.warnings( ).size( ) == 1 );
		CHECK( project_engine.warnings( ).front( ).find( "cannot widen" ) != std::string::npos );

		setup.approval.queue( perm::approval_outcome::deny_once );
		CHECK( project_engine.decide( { std::string{ "bash" }, tool_class::exec,
			std::string{ }, std::string{ "rm -rf /" } } ) == perm::permission_decision::deny );

		CHECK( project_engine.decide( { std::string{ "bash" }, tool_class::exec,
			std::string{ }, std::string{ "npm test" } } ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "the store survives a crash between temp-write and rename",
		"[perm][store]" ) {
		auto file = test::scratch_directory( "mcode-perm-crash" ) / "permissions.json";

		{
			auto setup = rig{ };
			auto store = perm::remember_store{ file };
			auto engine = perm::permission_engine{ setup.space, &store };
			engine.set_approval_source( &setup.approval );

			setup.approval.queue( perm::approval_outcome::allow_remember );
			REQUIRE( engine.decide( { std::string{ "bash" }, tool_class::exec,
				std::string{ }, std::string{ "git status" } } )
				== perm::permission_decision::allow );
		}

		write_store_file( file.parent_path( ) / "permissions.json.mcode-tmp-crash",
			R"({"version":1,"exec":{"garbage":"allow"}})" );

		auto reloaded = perm::remember_store{ file };
		const auto layer = reloaded.load( );
		REQUIRE( layer.has_value( ) );
		REQUIRE( layer->has_value( ) );

		const auto found = ( **layer ).exec.find( "git status" );
		REQUIRE( found != ( **layer ).exec.end( ) );
		CHECK( found->second == perm::store_decision::allow );
	}

	TEST_CASE( "the store keys are canonical: spacing and path spelling collapse",
		"[perm][store]" ) {
		CHECK( perm::canonical_argv( { "git", "status" } ) == "git status" );

		auto setup = rig{ };

		const auto tokens = perm::parse_command_line( "git   status" );
		REQUIRE( tokens.has_value( ) );

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.exec( perm::canonical_argv( *tokens ) )
			== perm::permission_decision::allow );

		const auto layer = setup.store.load( );
		REQUIRE( layer.has_value( ) );
		REQUIRE( layer->has_value( ) );
		CHECK( ( **layer ).exec.find( "git status" ) != ( **layer ).exec.end( ) );
	}

	TEST_CASE( "the remember store round-trips all three sections", "[perm][store]" ) {
		auto file = test::scratch_directory( "mcode-perm-roundtrip" ) / "permissions.json";

		auto store = perm::remember_store{ file };

		auto additions = perm::store_layer{ };
		additions.exec.insert_or_assign( "git status", perm::store_decision::allow );
		additions.exec.insert_or_assign( "npm install", perm::store_decision::deny );
		additions.paths.insert_or_assign( "C:/Users/u/notes/*", perm::store_decision::allow );
		additions.tools.insert_or_assign( "mcp__github__create_issue",
			perm::store_decision::allow );

		REQUIRE( store.save( additions ).has_value( ) );

		auto more = perm::store_layer{ };
		more.exec.insert_or_assign( "cargo test", perm::store_decision::allow );
		REQUIRE( store.save( more ).has_value( ) );

		const auto loaded = store.load( );
		REQUIRE( loaded.has_value( ) );
		REQUIRE( loaded->has_value( ) );

		CHECK( ( **loaded ).exec.size( ) == 3 );
		CHECK( ( **loaded ).exec.at( "git status" ) == perm::store_decision::allow );
		CHECK( ( **loaded ).exec.at( "npm install" ) == perm::store_decision::deny );
		CHECK( ( **loaded ).exec.at( "cargo test" ) == perm::store_decision::allow );
		CHECK( ( **loaded ).paths.size( ) == 1 );
		CHECK( ( **loaded ).tools.size( ) == 1 );
	}

	TEST_CASE( "an unreadable store fails closed, not empty", "[perm][store]" ) {
		auto file = test::scratch_directory( "mcode-perm-bad" ) / "permissions.json";
		write_store_file( file, "{not json" );

		auto store = perm::remember_store{ file };
		const auto loaded = store.load( );

		CHECK( !loaded.has_value( ) );
	}

	TEST_CASE( "a missing store is the normal first-run case", "[perm][store]" ) {
		auto file = test::scratch_directory( "mcode-perm-missing" ) / "permissions.json";

		auto store = perm::remember_store{ file };
		const auto loaded = store.load( );

		REQUIRE( loaded.has_value( ) );
		CHECK_FALSE( loaded->has_value( ) );
	}

	TEST_CASE( "an mcp call remembers per tool, not per arguments blob", "[perm][store][mcp]" ) {
		// the `tools` section is keyed by tool name, never by an MCP call's arguments resource.
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.mcp( "mcp__echo__read", R"({"path":"a.txt"})" )
			== perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );

		CHECK( setup.mcp( "mcp__echo__read", R"({"path":"b.txt"})" )
			== perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );

		CHECK( setup.mcp( "mcp__echo__write", R"({"path":"a.txt"})" )
			== perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 2 );
	}

	TEST_CASE( "a remembered mcp tool survives into a fresh engine", "[perm][store][mcp]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::allow );

		auto second = perm::permission_engine{ setup.space, &setup.store };
		second.set_approval_source( &setup.approval );
		CHECK( second.load_store( ).has_value( ) );

		auto request = perm::permission_request{ };
		request.tool_name = "mcp__echo__read";
		request.klass = tool_class::mcp;
		request.resource = "{}";

		CHECK( second.decide( request ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );
	}

	TEST_CASE( "the exec and tools sections never borrow each other's key",
		"[perm][store][near-miss]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.exec( "mcp__echo__read" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );

		CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 2 );
	}

	TEST_CASE( "a hand-written tools entry is honoured", "[perm][store][mcp]" ) {
		auto setup = rig{ };

		write_store_file( setup.store.file( ),
			R"({"version":1,"tools":{"mcp__echo__read":"allow"}})" );

		REQUIRE( setup.engine.load_store( ).has_value( ) );
		CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );
	}

} // namespace permission_test
