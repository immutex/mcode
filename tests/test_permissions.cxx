#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mcode/cli/exec.hxx"
#include "mcode/perm/argv.hxx"
#include "mcode/platform/seams.hxx"

#include "permission_test_helpers.hxx"

namespace permission_test {

	TEST_CASE( "an exec call with no allowlist prompts and the answer is honoured",
		"[perm][prompt]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::allow_once );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );

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

		auto denied = rig{ };
		denied.engine.add_config_rules( perm::rule_scope::user, { outside_text }, { } );
		CHECK( denied.read( outside_text ) == perm::permission_decision::deny );
		CHECK( denied.approval.asks( ) == 0 );

		auto always = rig{ false, "always" };
		always.approval.queue( perm::approval_outcome::allow_once );
		CHECK( always.read( outside_text ) == perm::permission_decision::allow );
		CHECK( always.approval.asks( ) == 1 );

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

		CHECK( widened.read( inside_added ) == perm::permission_decision::allow );
		CHECK( widened.approval.asks( ) == 0 );

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

		// writes outside the workspace are never auto-persisted.
		CHECK_FALSE( std::filesystem::exists( setup.store.file( ) ) );

		// the answer is a session allow, so the same request does not prompt again.
		CHECK( setup.write( target ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 1 );
	}

	TEST_CASE( "a hand-written paths entry is enforced, not just re-rendered",
		"[perm][store]" ) {
		auto setup = rig{ };

		const auto outside = test::scratch_directory( "mcode-perm-paths" );
		const auto notes = ( outside / "notes" / "a.txt" ).generic_string( );
		const auto secrets = ( outside / "secrets" / "key.txt" ).generic_string( );

		const auto file = test::scratch_directory( "mcode-perm-paths-store" ) / "permissions.json";
		write_store_file( file, "{\"version\":1,\"paths\":{\"" + notes + "\":\"allow\",\""
			+ secrets + "\":\"deny\"}}" );

		auto store = perm::remember_store{ file };
		auto engine = perm::permission_engine{ setup.space, &store };
		engine.set_approval_source( &setup.approval );

		REQUIRE( engine.load_store( ).has_value( ) );

		auto allowed = perm::permission_request{ };
		allowed.tool_name = "read";
		allowed.klass = tool_class::read;
		allowed.resource = notes;

		// without the store entry this read is outside the workspace and would prompt.
		CHECK( engine.decide( allowed ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );

		auto denied = perm::permission_request{ };
		denied.tool_name = "read";
		denied.klass = tool_class::read;
		denied.resource = secrets;

		CHECK( engine.decide( denied ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "headless fails closed and the model is told why", "[perm][headless]" ) {
		auto setup = rig{ };

		auto options = perm::permission_engine::options{ };
		options.headless = true;
		setup.engine.set_options( options );

		CHECK( setup.exec( "git push" ) == perm::permission_decision::deny );

		const auto& verdict = setup.engine.last_verdict( );
		CHECK( verdict.reason.find( "headless" ) != std::string::npos );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "yolo allows an ask but still denies the floor", "[perm][yolo]" ) {
		auto setup = rig{ true };

		CHECK( setup.exec( "git push --force origin main" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );

		// the floor is checked ahead of the rule merge, so a managed-scope allow cannot lift it.
		CHECK( setup.exec( "rm -rf ~" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "rm -rf ./build" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "plan mode denies every mutating class ahead of rules and yolo",
		"[perm][plan]" ) {
		auto setup = rig{ true };

		auto options = perm::permission_engine::options{ };
		options.yolo = true;
		options.plan_mode = true;
		setup.engine.set_options( options );

		// a user allow cannot lift it: plan mode decides ahead of the rule merge.
		setup.engine.add_config_rules( perm::rule_scope::user, { }, { "git status" } );

		CHECK( setup.exec( "git status" ) == perm::permission_decision::deny );
		CHECK( setup.engine.last_verdict( ).reason.find( "plan mode" ) != std::string::npos );

		// a write inside the workspace is still a write.
		CHECK( setup.write( "src/new-file.cxx" ) == perm::permission_decision::deny );

		auto net_request = perm::permission_request{ };
		net_request.tool_name = "fetch";
		net_request.klass = tool_class::net;
		net_request.resource = "https://example.com";
		CHECK( setup.engine.decide( net_request ) == perm::permission_decision::deny );

		auto spawn_request = perm::permission_request{ };
		spawn_request.tool_name = "spawn";
		spawn_request.klass = tool_class::spawn;
		spawn_request.resource = "worker";
		CHECK( setup.engine.decide( spawn_request ) == perm::permission_decision::deny );

		// mcp is not a read class, so it is refused too rather than escaping via yolo.
		CHECK( setup.mcp( "write_file", "{\"path\":\"a.txt\"}" )
			== perm::permission_decision::deny );

		// reads are untouched: looking without touching is the point.
		CHECK( setup.read( "src/main.cxx" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "the ask and plan flags parse as booleans and ask beats yolo",
		"[perm][argv]" ) {
		auto plan = cli::parse_exec_options( { "--plan" } );
		REQUIRE( plan.has_value( ) );
		CHECK( plan->plan );
		CHECK_FALSE( plan->ask );
		CHECK_FALSE( plan->yolo );
		CHECK( plan->unknown_arguments.empty( ) );

		auto ask = cli::parse_exec_options( { "--ask" } );
		REQUIRE( ask.has_value( ) );
		CHECK( ask->ask );
		CHECK_FALSE( ask->yolo );

		// whichever order the two arrive in, the prompt is restored.
		auto ask_then_yolo = cli::parse_exec_options( { "--ask", "--yolo" } );
		REQUIRE( ask_then_yolo.has_value( ) );
		CHECK( ask_then_yolo->ask );
		CHECK_FALSE( ask_then_yolo->yolo );

		auto yolo_then_ask = cli::parse_exec_options( { "--yolo", "--ask" } );
		REQUIRE( yolo_then_ask.has_value( ) );
		CHECK( yolo_then_ask->ask );
		CHECK_FALSE( yolo_then_ask->yolo );

		// --plan is independent: it does not disturb the other flags.
		auto both = cli::parse_exec_options( { "--plan", "--yolo" } );
		REQUIRE( both.has_value( ) );
		CHECK( both->plan );
		CHECK( both->yolo );

		auto approval = cli::parse_exec_options( { "--approval", "always" } );
		REQUIRE( approval.has_value( ) );
		CHECK( approval->approval == "always" );
		CHECK_FALSE( approval->ask );
		CHECK_FALSE( approval->plan );
	}

	TEST_CASE( "an unparsable or compound command is an ask, never an allow",
		"[perm][argv]" ) {
		auto setup = rig{ };

		// an unparsable command is an empty resource no rule may match, so it denies by default.
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

		const auto& detail = setup.approval.last_detail( );
		CHECK( detail.find( "git push --force origin main" ) != std::string::npos );
		CHECK( detail.find( "cwd:" ) != std::string::npos );
		CHECK( detail.find( "rule:" ) != std::string::npos );

		CHECK( setup.approval.last_request( ).subject == "git push --force origin main" );
		CHECK( setup.approval.last_request( ).action == "run" );
	}

	TEST_CASE( "[d] is a session-scope deny that outranks a later allow", "[perm][prompt]" ) {
		auto setup = rig{ };

		setup.approval.queue( perm::approval_outcome::deny_session );
		CHECK( setup.exec( "terraform destroy" ) == perm::permission_decision::deny );

		setup.engine.add_config_rules( perm::rule_scope::user, { }, { "terraform destroy" } );

		CHECK( setup.exec( "terraform destroy" ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 1 );

		// the deny is session-scoped, so nothing reached the store file.
		CHECK_FALSE( std::filesystem::exists( setup.store.file( ) ) );
	}

	TEST_CASE( "an engine with no approval source fails closed", "[perm][headless]" ) {
		auto setup = rig{ };

		// a fresh engine over the same space, with no source wired at all.
		auto engine = perm::permission_engine{ setup.space, &setup.store };

		auto request = perm::permission_request{ };
		request.tool_name = "bash";
		request.klass = tool_class::exec;
		request.resource = "git status";

		CHECK( engine.decide( request ) == perm::permission_decision::deny );
		CHECK( engine.last_verdict( ).reason.find( "headless" ) != std::string::npos );
	}

	TEST_CASE( "an approval source that cannot answer fails closed", "[perm][prompt]" ) {
		auto setup = rig{ };

		// the scripted source returns `refused` once its queue is empty.
		CHECK( setup.exec( "git status" ) == perm::permission_decision::deny );
		CHECK( setup.approval.asks( ) == 1 );
		CHECK( setup.engine.last_verdict( ).reason.find( "failing closed" ) != std::string::npos );

		setup.approval.queue( perm::approval_outcome::detail );
		CHECK( setup.exec( "git status" ) == perm::permission_decision::deny );
		CHECK( setup.engine.last_verdict( ).reason.find( "failing closed" ) != std::string::npos );
	}

	TEST_CASE( "exit-5 plumbing: the loop's flag clears on a later success",
		"[perm][loop]" ) {
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

	TEST_CASE( "a quoted metacharacter is data, not a shell operator", "[perm][argv]" ) {
		// Nothing runs a shell: the tokens become argv directly. So a metacharacter
		// inside quotes is a literal byte the program receives, and refusing it
		// rejected real commands. A measured run lost five turns to this: every
		// `python3 -c '...; ...'` one-liner and a `grep -A 34 '<header>'` were
		// refused as "compound", and the model fell back to weaker tools.
		const auto quoted = perm::parse_command_line(
			R"(grep -n -A 34 '<header class="site-header">' src/index.html)" );

		REQUIRE( quoted.has_value( ) );
		REQUIRE( quoted->size( ) == 6 );
		CHECK( quoted->at( 0 ) == "grep" );
		CHECK( quoted->at( 1 ) == "-n" );
		CHECK( quoted->at( 3 ) == "34" );

		// The quotes are stripped and the inner double quotes are preserved.
		CHECK( quoted->at( 4 ) == R"(<header class="site-header">)" );
		CHECK( quoted->at( 5 ) == "src/index.html" );

		// A one-liner whose semicolon is inside the quotes.
		const auto script = perm::parse_command_line(
			R"(python3 -c 'import re; print(re.sub("a", "b", "a"))')" );

		REQUIRE( script.has_value( ) );
		CHECK( script->size( ) == 3 );
		CHECK( script->at( 2 ) == R"(import re; print(re.sub("a", "b", "a")))" );

		// A multi-line script argument is still one token.
		const auto multiline = perm::parse_command_line(
			std::string{ "python3 -c 'line one\nline two'" } );

		REQUIRE( multiline.has_value( ) );
		CHECK( multiline->at( 2 ) == "line one\nline two" );

		// A quoted dollar is literal too.
		const auto literal_dollar = perm::parse_command_line( R"(echo '$HOME')" );

		REQUIRE( literal_dollar.has_value( ) );
		CHECK( literal_dollar->at( 1 ) == "$HOME" );
	}

	TEST_CASE( "an unquoted operator is still refused", "[perm][argv]" ) {
		// The guard the fix must not weaken: an unquoted operator means the model
		// expected a shell, and no shell runs, so its intent would silently not
		// happen. These are the cases the gate exists for.
		CHECK_FALSE( perm::parse_command_line( "echo a; echo b" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "echo a && echo b" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "cat a | grep b" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "git log > out.txt" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "echo `whoami`" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "echo $HOME" ).has_value( ) );
		CHECK_FALSE( perm::parse_command_line( "echo %PATH%" ).has_value( ) );

		// A metacharacter after a closed quote is unquoted and still refused.
		CHECK_FALSE( perm::parse_command_line( R"(echo 'a' && echo b)" ).has_value( ) );
	}

} // namespace permission_test

	TEST_CASE( "an escaped quote inside double quotes does not end the string", "[perm][argv]" ) {
		// The exact command a measured run was refused. The `\"` sequences closed
		// the quote early, so the `||` after them was scanned as unquoted and read
		// as a pipe -- the command was rejected as "compound" when nothing about it
		// was compound. The model lost a turn and had to work around the gate.
		const auto node = perm::parse_command_line(
			R"(node -e "const m='x'; console.log(m.match(/id=\"[^\"]+\"/g)||[]);")" );

		REQUIRE( node.has_value( ) );
		REQUIRE( node->size( ) == 3 );
		CHECK( node->at( 0 ) == "node" );
		CHECK( node->at( 1 ) == "-e" );
		CHECK( node->at( 2 ) == R"(const m='x'; console.log(m.match(/id="[^"]+"/g)||[]);)" );

		// A backslash before an ordinary character is literal, so a Windows path
		// survives. Only the characters sh escapes are consumed.
		const auto path = perm::parse_command_line( R"(node "C:\Users\somebody\x.js")" );

		REQUIRE( path.has_value( ) );
		CHECK( path->at( 1 ) == R"(C:\Users\somebody\x.js)" );

		// A doubled backslash is one backslash, as in sh.
		const auto doubled = perm::parse_command_line( R"(node -e "a\\b")" );

		REQUIRE( doubled.has_value( ) );
		CHECK( doubled->at( 2 ) == R"(a\b)" );

		// Single quotes take no escapes: a backslash stays, and the quote inside
		// cannot end the string.
		const auto single = perm::parse_command_line( R"(node -e 'a\"b')" );

		REQUIRE( single.has_value( ) );
		CHECK( single->at( 2 ) == R"(a\"b)" );
	}

	TEST_CASE( "an unterminated escape at the end of a string is refused", "[perm][argv]" ) {
		// A trailing backslash inside a quote has nothing to escape, so the quote
		// never closes and the command cannot be judged.
		CHECK_FALSE( perm::parse_command_line( R"(node -e "abc\)" ).has_value( ) );
	}

	TEST_CASE( "the sandbox temp directory is our own, not the user's", "[perm][sandbox]" ) {
		// The bug this guards: the child was granted the user's whole %TEMP%. On
		// Windows a write grant is an integrity label, which propagates to every
		// entry beneath the path, so each bash call walked and relabelled the
		// entire temp tree -- 20.7 s of a 21.0 s spawn, and a lasting change to
		// files unrelated to the run.
		const auto system_temp = mcode::platform::temp_directory( );

		REQUIRE( system_temp.has_value( ) );

		const auto ours = mcode::platform::sandbox_temp_directory( );

		REQUIRE( ours.has_value( ) );

		// Inside the system temp, but a subdirectory of it -- never the root itself.
		// `temp_directory_path` returns a path with a trailing separator, and
		// `lexically_normal` keeps it on a root path, so it is stripped for the
		// comparison rather than relied on to be absent.
		const auto strip = []( std::string text ) {
			while ( !text.empty( ) && ( text.back( ) == '\\' || text.back( ) == '/' ) ) {
				text.pop_back( );
			}

			return text;
		};

		CHECK( strip( ours->parent_path( ).string( ) ) == strip( system_temp->string( ) ) );
		CHECK( *ours != *system_temp );
		CHECK( ours->filename( ).string( ).starts_with(
			std::string{ mcode::platform::SANDBOX_TEMP_PREFIX } ) );

		// Usable: created, and a directory.
		auto error = std::error_code{ };
		CHECK( std::filesystem::is_directory( *ours, error ) );
		CHECK_FALSE( static_cast< bool >( error ) );

		// Stable across calls, so a session leaves one directory rather than one
		// per command.
		const auto again = mcode::platform::sandbox_temp_directory( );

		REQUIRE( again.has_value( ) );
		CHECK( again->string( ) == ours->string( ) );
	}
