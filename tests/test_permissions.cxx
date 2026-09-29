// The permission engine, the approval prompt, the remember store and the
// hard-deny floor. Every item in the slice's acceptance list is asserted here,
// including the floor's near-miss table in both directions.

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "mcode/fs/workspace.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/perm/argv.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	// The scripted source from the tools helpers, re-declared here so this file
	// stays independent of tools_test_helpers (which pulls in the tool layer).
	class scripted_source final : public perm::approval_source {
	public:
		auto queue( const perm::approval_outcome answer ) -> void {
			answers_.push_back( answer );
		}

		[[nodiscard]] auto asks( ) const noexcept -> std::size_t {
			return asks_;
		}

		[[nodiscard]] auto last_request( ) const -> const perm::approval_request& {
			return last_request_;
		}

		[[nodiscard]] auto ask( const perm::approval_request& request,
			const std::function< std::string( ) >& detail ) -> perm::approval_outcome override {
			++asks_;
			last_request_ = request;
			last_detail_ = detail( );

			if ( answers_.empty( ) ) {
				return perm::approval_outcome::refused;
			}

			const auto answer = answers_.front( );
			answers_.pop_front( );

			return answer;
		}

		[[nodiscard]] auto last_detail( ) const -> const std::string& {
			return last_detail_;
		}

	private:
		std::deque< perm::approval_outcome > answers_;
		std::size_t asks_ = 0;
		perm::approval_request last_request_;
		std::string last_detail_;
	};

	struct rig {
		std::filesystem::path path;
		workspace space;
		perm::remember_store store;
		scripted_source approval;
		perm::permission_engine engine;

		explicit rig( const bool yolo = false, const std::string approval_mode = "on-request" )
			: space( workspace::open( make_root( ) ).value( ) ),
			store( test::scratch_directory( "mcode-perm" ) / "permissions.json" ),
			engine( space, &store ) {
			path = space.root( );

			std::filesystem::create_directories( path / "src" );

			auto options = perm::permission_engine::options{ };
			options.yolo = yolo;
			options.approval = approval_mode;
			engine.set_options( options );
			engine.set_approval_source( &approval );
		}

		[[nodiscard]] static auto make_root( ) -> std::filesystem::path {
			return test::scratch_directory( "mcode-perm-ws" );
		}

		auto exec( const std::string& argv ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "bash";
			request.klass = tool_class::exec;
			request.resource = argv;

			return engine.decide( request );
		}

		auto read( const std::string& path_text ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "read";
			request.klass = tool_class::read;
			request.resource = path_text;

			return engine.decide( request );
		}

		auto write( const std::string& path_text ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "write";
			request.klass = tool_class::write;
			request.resource = path_text;

			return engine.decide( request );
		}

		// An MCP tool: its resource is an opaque arguments blob, so the store
		// must key it by name rather than by what it was called with.
		auto mcp( const std::string& tool, const std::string& arguments ) {
			auto request = perm::permission_request{ };
			request.tool_name = tool;
			request.klass = tool_class::mcp;
			request.owner = "mcp:echo";
			request.resource = arguments;

			return engine.decide( request );
		}
	};

	// Builds a store file with the given JSON body, for the project-store tests.
	auto write_store_file( const std::filesystem::path& file, const std::string& body ) -> void {
		std::filesystem::create_directories( file.parent_path( ) );

		auto out = std::ofstream{ file, std::ios::binary };
		out << body;
	}

}

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

TEST_CASE( "an allow-once answer does not persist; always does",
	"[perm][prompt][store]" ) {
	auto setup = rig{ };

	setup.approval.queue( perm::approval_outcome::allow_once );
	CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );

	// Same argv again in the same session prompts again: allow_once is not
	// remembered.
	setup.approval.queue( perm::approval_outcome::allow_once );
	CHECK( setup.exec( "git status" ) == perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 2 );

	// `always` persists. The second identical command does not prompt.
	setup.approval.queue( perm::approval_outcome::allow_remember );
	CHECK( setup.exec( "npm install" ) == perm::permission_decision::allow );

	// A fresh engine over the SAME store file: the persisted answer is
	// re-read, not re-asked.
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

	// `git status` remembered does not authorize `git push`, or any other
	// argv. This is the bypass class the security doc names.
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

	// A second engine over the same store file, fresh process semantics.
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

TEST_CASE( "a project store cannot widen: allows are dropped with a warning",
	"[perm][store]" ) {
	// The store must sit under the workspace root for the engine to classify
	// it as project-scope.
	auto setup = rig{ };
	auto file = setup.path / ".mcode" / "permissions.json";
	write_store_file( file,
		R"({"version":1,"exec":{"rm -rf /":"allow","npm test":"deny"}})" );

	auto project_store = perm::remember_store{ file };
	auto project_engine = perm::permission_engine{ setup.space, &project_store };
	project_engine.set_approval_source( &setup.approval );

	const auto loaded = project_engine.load_store( );
	REQUIRE( loaded.has_value( ) );

	// The allow is dropped -- a cloned repo must not be able to widen its own
	// permissions. The deny survives. The drop is warned, not silent.
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

	// The crash: a temp file is written beside the store and the process dies
	// before the rename. The previous contents must be intact. Simulate the
	// crash by writing a temp file manually and NOT renaming it.
	write_store_file( file.parent_path( ) / "permissions.json.mcode-tmp-crash",
		R"({"version":1,"exec":{"garbage":"allow"}})" );

	// The real store still parses and still holds the remembered answer.
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

	// The tool layer parses and canonicalizes before the engine sees the
	// resource; the same round trip here.
	const auto tokens = perm::parse_command_line( "git   status" );
	REQUIRE( tokens.has_value( ) );

	setup.approval.queue( perm::approval_outcome::allow_remember );
	CHECK( setup.exec( perm::canonical_argv( *tokens ) )
		== perm::permission_decision::allow );

	// The store now holds the canonical single-space form.
	const auto layer = setup.store.load( );
	REQUIRE( layer.has_value( ) );
	REQUIRE( layer->has_value( ) );
	CHECK( ( **layer ).exec.find( "git status" ) != ( **layer ).exec.end( ) );
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

TEST_CASE( "the hard-deny floor: denied commands and their legitimate near-misses",
	"[perm][floor]" ) {
	auto setup = rig{ true };

	// Denied: recursive delete of a root.
	CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
	CHECK( setup.exec( "rm -rf //" ) == perm::permission_decision::deny );
	CHECK( setup.exec( "rm -rf /./" ) == perm::permission_decision::deny );

	// Allowed: the near-misses.
	CHECK( setup.exec( "rm -rf ./build" ) == perm::permission_decision::allow );
	CHECK( setup.exec( "rm -rf node_modules" ) == perm::permission_decision::allow );

	// Denied: recursive delete of the home.
	CHECK( setup.exec( "rm -rf ~" ) == perm::permission_decision::deny );

	// Allowed: a path UNDER the home is not the home.
	CHECK( setup.exec( "rm -rf ~/scratch/project-a" ) == perm::permission_decision::allow );

	// Denied: privilege escalation as the first token.
	CHECK( setup.exec( "sudo apt install x" ) == perm::permission_decision::deny );

	// Allowed: the word as an argument.
	CHECK( setup.exec( "echo sudo" ) == perm::permission_decision::allow );
	CHECK( setup.exec( "grep sudo README.md" ) == perm::permission_decision::allow );

	// Denied: raw write to a device.
	CHECK( setup.exec( "dd if=/dev/zero of=/dev/sda" ) == perm::permission_decision::deny );

	// Allowed: a file target.
	CHECK( setup.exec( "dd if=/dev/zero of=./image.img bs=1M count=10" )
		== perm::permission_decision::allow );

	// Denied: filesystem creation.
	CHECK( setup.exec( "mkfs.ext4 /dev/sdb1" ) == perm::permission_decision::deny );

	// Allowed: the word as an argument to something else.
	CHECK( setup.exec( "man mkfs.ext4" ) == perm::permission_decision::allow );
}

TEST_CASE( "a write to .git internals is denied; .gitignore is not", "[perm][floor]" ) {
	auto setup = rig{ };

	CHECK( setup.write( ".git/config" ) == perm::permission_decision::deny );
	CHECK( setup.write( ".mcode/config.toml" ) == perm::permission_decision::deny );

	// The near-miss: .gitignore merely starts with the same characters. A
	// prefix match here would refuse every .gitignore edit.
	CHECK( setup.write( ".gitignore" ) == perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 0 );
}

TEST_CASE( "the floor cannot be overridden by a config rule", "[perm][floor]" ) {
	auto setup = rig{ };

	// Even a managed-scope allow for the exact argv must not lift the floor:
	// the floor is a separate check ahead of the rule merge, not a rule.
	setup.engine.add_config_rules( perm::rule_scope::managed, { }, { "rm -rf /" } );

	CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
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

	// A second save must not erase the first's entries.
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


TEST_CASE( "an mcp call remembers per tool, not per arguments blob", "[perm][store][mcp]" ) {
	// The store's `tools` section is keyed by tool name. The resource for an
	// MCP call is its arguments JSON, so a lookup keyed on the resource could
	// never match an entry keyed by name -- and `allow_remember` did not write
	// one at all, so "always" persisted nothing and the next identical call
	// prompted again.
	auto setup = rig{ };

	setup.approval.queue( perm::approval_outcome::allow_remember );
	CHECK( setup.mcp( "mcp__echo__read", R"({"path":"a.txt"})" )
		== perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 1 );

	// Same tool, different arguments: still no prompt, because the key is the
	// tool name and not the arguments.
	CHECK( setup.mcp( "mcp__echo__read", R"({"path":"b.txt"})" )
		== perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 1 );

	// A different tool is a different question.
	CHECK( setup.mcp( "mcp__echo__write", R"({"path":"a.txt"})" )
		== perm::permission_decision::deny );
	CHECK( setup.approval.asks( ) == 2 );
}

TEST_CASE( "a remembered mcp tool survives into a fresh engine", "[perm][store][mcp]" ) {
	auto setup = rig{ };

	setup.approval.queue( perm::approval_outcome::allow_remember );
	CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::allow );

	// A second engine over the same store file: the answer was written, so the
	// question is not asked again. This is the "zero prompts on a repeat run"
	// half of the acceptance criterion.
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
	// If the two ever collapse into one key space, a remembered command would
	// authorize a tool call with the same name, or the reverse. Both halves.
	auto setup = rig{ };

	setup.approval.queue( perm::approval_outcome::allow_remember );
	CHECK( setup.exec( "mcp__echo__read" ) == perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 1 );

	// The same string as a tool name is a different subject and must prompt.
	CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::deny );
	CHECK( setup.approval.asks( ) == 2 );
}

TEST_CASE( "a hand-written tools entry is honoured", "[perm][store][mcp]" ) {
	// The section is readable and hand-editable, which is the point of a
	// separate JSON file rather than a TOML table.
	auto setup = rig{ };

	write_store_file( setup.store.file( ),
		R"({"version":1,"tools":{"mcp__echo__read":"allow"}})" );

	REQUIRE( setup.engine.load_store( ).has_value( ) );
	CHECK( setup.mcp( "mcp__echo__read", "{}" ) == perm::permission_decision::allow );
	CHECK( setup.approval.asks( ) == 0 );
}

TEST_CASE( "the protected-path floor denies .git internals and not .gitignore",
	"[perm][floor][near-miss]" ) {
	// The near-miss that matters: `is_protected` matches the `.git/` directory,
	// and `.gitignore` merely starts with the same characters. A prefix match
	// would refuse every `.gitignore` edit, a file the agent edits constantly.
	//
	// The fail-closed branch for a path that cannot be canonicalized is not
	// asserted here: on this platform `weakly_canonical` resolves every input
	// reachable from a test, so there is no input that reaches it. It is
	// defensive, and it is recorded as uncovered rather than tested with an
	// assertion that cannot fail.
	auto setup = rig{ };

	auto protected_path = perm::permission_request{ };
	protected_path.tool_name = "write";
	protected_path.klass = tool_class::write;
	protected_path.resource = ".git/config";

	CHECK( setup.engine.on_floor( protected_path ).has_value( ) );
	CHECK( setup.engine.decide( protected_path ) == perm::permission_decision::deny );

	auto ordinary = perm::permission_request{ };
	ordinary.tool_name = "write";
	ordinary.klass = tool_class::write;
	ordinary.resource = ".gitignore";

	CHECK_FALSE( setup.engine.on_floor( ordinary ).has_value( ) );
	CHECK( setup.engine.decide( ordinary ) == perm::permission_decision::allow );
}

TEST_CASE( "a quoted token with a space is not re-split by the floor",
	"[perm][floor][near-miss]" ) {
	// `on_floor` used to re-split the canonical argv on spaces, so one token
	// containing a space became two and the target resolved to a path nobody
	// named. The near-miss is that a genuine root delete still fires.
	auto setup = rig{ };

	auto ordinary = perm::permission_request{ };
	ordinary.tool_name = "bash";
	ordinary.klass = tool_class::exec;
	ordinary.resource = R"(rm -rf "my dir")";

	CHECK_FALSE( setup.engine.on_floor( ordinary ).has_value( ) );

	auto root_delete = perm::permission_request{ };
	root_delete.tool_name = "bash";
	root_delete.klass = tool_class::exec;
	root_delete.resource = "rm -rf /";

	CHECK( setup.engine.on_floor( root_delete ).has_value( ) );
}
