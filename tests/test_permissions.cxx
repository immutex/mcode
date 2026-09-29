// The permission engine, the approval prompt and the modes. The hard-deny
// floor lives in test_floor.cxx and the remember store in
// test_permission_store.cxx.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mcode/perm/argv.hxx"

#include "permission_test_helpers.hxx"

namespace permission_test {

	TEST_CASE( "an exec call with no allowlist prompts and the answer is honoured",
		"[perm][prompt]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );

		// A deny answer is honoured too.
		setup.approval.queue( perm::approval_outcome::deny_once );
		CHECK( setup.exec( "git push" ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 2 );
	}

	TEST_CASE( "a deny outranks an allow regardless of scope order", "[perm][engine]" ) {
		auto setup = rig{ };

		setup.engine.add_config_rules( perm::rule_scope::user, { }, { "git status" } );
		setup.engine.add_config_rules( perm::rule_scope::session, { "git status" }, { } );

		CHECK( setup.exec( "git status" ) == perm::permission_decision::deny );

		auto swapped = rig{ };
		swapped.engine.add_config_rules( perm::rule_scope::user, { "git status" }, { } );
		swapped.engine.add_config_rules( perm::rule_scope::session, { }, { "git status" } );

		CHECK( swapped.exec( "git status" ) == perm::permission_decision::deny );
	}

	TEST_CASE( "an outside-workspace read is a decision, not a hard refusal",
		"[perm][boundary]" ) {
		auto outside = test::scratch_directory( "mcode-perm-outside" );
		std::filesystem::create_directories( outside );
		const auto outside_file = outside / "secret.txt";

		{
			auto out = std::ofstream{ outside_file, std::ios::binary };
			out << "content";
		}

		const auto outside_text = outside_file.generic_string( );

		// 1. A deny rule denies it without prompting.
		auto denied = rig{ };
		denied.engine.add_config_rules( perm::rule_scope::user, { outside_text }, { } );
		CHECK( denied.read( outside_text ) == perm::permission_decision::deny );
		CHECK( denied.approval.asks( ) == 0 );

		// 2. approval = always prompts.
		auto always = rig{ false, "always" };
		always.approval.queue( perm::approval_outcome::allow_once );
		CHECK( always.read( outside_text ) == perm::permission_decision::allow );
		CHECK( always.approval.asks( ) == 1 );

		// 3. The default set prompts and the answer is honoured.
		auto prompted = rig{ };
		prompted.approval.queue( perm::approval_outcome::deny_once );
		CHECK( prompted.read( outside_text ) == perm::permission_decision::deny );
		CHECK( prompted.approval.asks( ) == 1 );
	}

	TEST_CASE( "add-dir widens the boundary for one run", "[perm][boundary]" ) {
		auto outside = test::scratch_directory( "mcode-perm-adddir" );
		std::filesystem::create_directories( outside / "sub" );
		const auto inside_added = ( outside / "sub" / "file.txt" ).generic_string( );

		auto widened = rig{ };
		widened.engine.add_root( outside );

		// Under the added root: no prompt, allowed as an in-boundary read.
		CHECK( widened.read( inside_added ) == perm::permission_decision::allow );
		CHECK( widened.approval.asks( ) == 0 );

		// The same read without the flag prompts.
		auto narrow = rig{ };
		narrow.approval.queue( perm::approval_outcome::deny_once );
		CHECK( narrow.read( inside_added ) == perm::permission_decision::deny );
		CHECK( narrow.approval.asks( ) == 1 );
	}

	TEST_CASE( "approval = always prompts even for an in-workspace read",
		"[perm][modes]" ) {
		auto setup = rig{ false, "always" };

		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.read( "src/main.cxx" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );
	}

	TEST_CASE( "workspace-internal writes are allowed without a prompt",
		"[perm][modes]" ) {
		auto setup = rig{ };

		CHECK( setup.write( "src/new-file.cxx" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "a write outside the workspace prompts and is never auto-persisted",
		"[perm][modes]" ) {
		auto outside = test::scratch_directory( "mcode-perm-outside-write" );
		const auto target = ( outside / "out.txt" ).generic_string( );

		auto setup = rig{ };
		setup.approval.queue( perm::approval_outcome::allow_remember );
		CHECK( setup.write( target ) == perm::permission_decision::allow );

		// The `always` answer must NOT have been persisted: writes outside the
		// workspace are never auto-persisted.
		const auto layer = setup.store.load( );
		REQUIRE( layer.has_value( ) );

		if ( layer->has_value( ) ) {
			CHECK( ( **layer ).paths.find( target ) == ( **layer ).paths.end( ) );
		}
	}

	TEST_CASE( "headless fails closed and the model is told why", "[perm][headless]" ) {
		auto setup = rig{ };

		auto options = perm::permission_engine::options{ };
		options.headless = true;
		setup.engine.set_options( options );

		// No approval source attached at all: the engine must deny, not crash.
		CHECK( setup.exec( "git push" ) == perm::permission_decision::deny );

		const auto& verdict = setup.engine.last_verdict( );
		CHECK( verdict.reason.find( "headless" ) != std::string::npos );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "yolo allows an ask but still denies the floor", "[perm][yolo]" ) {
		auto setup = rig{ true };

		// The ask resolves to allow without prompting.
		CHECK( setup.exec( "git push --force origin main" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );

		// The floor survives yolo. Both halves asserted; the second is the one a
		// careless implementation drops.
		CHECK( setup.exec( "rm -rf ~" ) == perm::permission_decision::deny );

		// And the near-miss is still allowed under yolo.
		CHECK( setup.exec( "rm -rf ./build" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "an unparsable or compound command is an ask, never an allow",
		"[perm][argv]" ) {
		auto setup = rig{ };

		// The loop maps an unparsable command to an empty resource. No rule may
		// match it -- even a user-authored allow for "" -- so it always resolves
		// through the default set's ask. With no answer available the resolution
		// is a deny, never an allow.
		setup.engine.add_config_rules( perm::rule_scope::user, { }, { "" } );

		CHECK( setup.exec( "" ) == perm::permission_decision::deny );
		CHECK( setup.engine.last_verdict( ).matched.scope == "default" );
		CHECK( setup.engine.last_verdict( ).matched.pattern ==
			"unparsable or compound command" );
	}

	TEST_CASE( "the [?] detail view shows enough to judge the request", "[perm][prompt]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::detail );
		setup.approval.queue( perm::approval_outcome::deny_once );

		CHECK( setup.exec( "git push --force origin main" ) == perm::permission_decision::deny );

		// The detail text reached the source: full argv, cwd, and the rule.
		const auto& detail = setup.approval.last_detail( );
		CHECK( detail.find( "git push --force origin main" ) != std::string::npos );
		CHECK( detail.find( "cwd:" ) != std::string::npos );
		CHECK( detail.find( "rule:" ) != std::string::npos );

		// The prompt itself names the action and the subject.
		CHECK( setup.approval.last_request( ).subject == "git push --force origin main" );
		CHECK( setup.approval.last_request( ).action == "run" );
	}

	TEST_CASE( "[d] is a session-scope deny that outranks a later allow", "[perm][prompt]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::deny_session );
		CHECK( setup.exec( "terraform destroy" ) == perm::permission_decision::deny );

		// The session deny outranks any later allow from a lower scope.
		setup.engine.add_config_rules( perm::rule_scope::user, { }, { "terraform destroy" } );

		CHECK( setup.exec( "terraform destroy" ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 1 );

		// And it was not persisted: [d] never writes the store.
		const auto layer = setup.store.load( );
		REQUIRE( layer.has_value( ) );

		if ( layer->has_value( ) ) {
			CHECK( ( **layer ).exec.find( "terraform destroy" ) == ( **layer ).exec.end( ) );
		}
	}

	TEST_CASE( "exit-5 plumbing: the loop's flag clears on a later success",
		"[perm][loop]" ) {
		// Covered end-to-end in test_loop; asserted here at the unit level through
		// the engine's verdict contract: a deny sets the reason, a later allow
		// replaces it.
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::deny_once );
		CHECK( setup.exec( "git push" ) == perm::permission_decision::deny );
		CHECK( setup.engine.last_verdict( ).decision == perm::permission_decision::deny );

		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );
		CHECK( setup.engine.last_verdict( ).decision == perm::permission_decision::allow );
	}

	TEST_CASE( "command parsing refusals still hold after the fold", "[perm][argv]" ) {
		CHECK_FALSE( perm::parse_command_line( "git log > .git/hooks/pre-commit" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "echo a && echo b" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( std::string{ "echo a\nrm -rf /" } ).has_value( ) );
		CHECK( perm::parse_command_line( "git status" ).value( )
			== std::vector< std::string >{ "git", "status" } );

		CHECK( perm::is_exec_runner( "/bin/sh" ) );
		CHECK( perm::is_exec_runner( "CMD" ) );
		CHECK( perm::is_exec_runner( "bash.exe" ) );
		CHECK_FALSE( perm::is_exec_runner( "git" ) );
		CHECK_FALSE( perm::is_exec_runner( "cmake" ) );
	}

} // namespace permission_test
