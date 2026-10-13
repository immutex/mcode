#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/message_json.hxx"
#include "mcode/cli/slash.hxx"
#include "mcode/model/types.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto commands( ) -> const std::vector< tui::slash_command >& {
		static const auto LIST = std::vector< tui::slash_command >{
			{ "help", "List the available commands" },
			{ "model", "Show the model" },
			{ "tools", "List the tools" },
			{ "remodel", "Re-read the model" },
		};

		return LIST;
	}

}

TEST_CASE( "a leading slash with no space is parsed as a command", "[cli][slash]" ) {
	const auto match = cli::match_command( "/help", commands( ) );

	CHECK( match.is_command );
	REQUIRE( match.entry != nullptr );
	CHECK( match.name == "help" );
	CHECK( match.arguments.empty( ) );
}

TEST_CASE( "everything after the first space is the argument string",
	"[cli][slash]" ) {
	const auto match = cli::match_command( "/help me now", commands( ) );

	CHECK( match.is_command );
	REQUIRE( match.entry != nullptr );
	CHECK( match.name == "help" );
	CHECK( match.arguments == "me now" );
}

TEST_CASE( "a line without a leading slash is not a command", "[cli][slash]" ) {
	CHECK_FALSE( cli::match_command( "hello", commands( ) ).is_command );
	CHECK_FALSE( cli::match_command( "@src/main.cxx", commands( ) ).is_command );
}

TEST_CASE( "an unknown name is still a command but resolves to no entry",
	"[cli][slash]" ) {
	const auto match = cli::match_command( "/unknown", commands( ) );

	CHECK( match.is_command );
	CHECK( match.entry == nullptr );
	CHECK( match.name == "unknown" );
}

TEST_CASE( "every built-in command resolves by its own name", "[cli][slash]" ) {
	const auto& builtins = cli::builtin_commands( );

	REQUIRE_FALSE( builtins.empty( ) );

	for ( const auto& entry : builtins ) {
		const auto match = cli::match_command( "/" + entry.name, builtins );

		CHECK( match.is_command );
		REQUIRE( match.entry != nullptr );
		CHECK( match.entry->name == entry.name );

		// A palette row with no description is a row the user cannot judge.
		CHECK_FALSE( entry.description.empty( ) );
	}
}

TEST_CASE( "the four new commands are offered", "[cli][slash]" ) {
	const auto& builtins = cli::builtin_commands( );

	for ( const auto* name : { "context", "compact", "export", "mention" } ) {
		const auto match = cli::match_command( std::string{ "/" } + name, builtins );

		REQUIRE( match.entry != nullptr );
	}
}

TEST_CASE( "the transcript export quotes every message and labels its blocks",
	"[cli][slash]" ) {
	auto history = std::vector< model::message >{ };

	auto task = model::message{ };
	task.speaker = model::role::user;

	auto task_block = model::block{ };
	task_block.kind = model::block_kind::text;
	task_block.text = "read the file";
	task.blocks.push_back( task_block );

	auto call = model::message{ };
	call.speaker = model::role::assistant;

	auto call_block = model::block{ };
	call_block.kind = model::block_kind::tool_call;
	call_block.tool_name = "read";
	call_block.args_json = R"({"path":"src/main.cxx"})";
	call.blocks.push_back( call_block );

	history.push_back( task );
	history.push_back( call );

	const auto markdown = cli::render_transcript_markdown( history );

	CHECK( markdown.starts_with( "# mcode session transcript" ) );
	CHECK( markdown.find( "## user" ) != std::string::npos );
	CHECK( markdown.find( "read the file" ) != std::string::npos );
	CHECK( markdown.find( "## assistant" ) != std::string::npos );
	CHECK( markdown.find( "**read**" ) != std::string::npos );
	CHECK( markdown.find( R"({"path":"src/main.cxx"})" ) != std::string::npos );
}

TEST_CASE( "an export argument names the file, inside the session's working directory",
	"[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-export" );

	auto deps = agent_loop::dependencies{ };
	deps.workspace_root = directory.string( );

	auto loop = agent_loop{ deps };
	const auto& builtins = cli::builtin_commands( );
	const auto match = cli::match_command( "/export notes.md", builtins );

	REQUIRE( match.entry != nullptr );

	const auto result = cli::run_command( match, loop, builtins );

	CHECK( result.output.find( "notes.md" ) != std::string::npos );
	CHECK( std::filesystem::exists( directory / "notes.md" ) );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the extensions command reports what loaded, what failed and what is disabled",
	"[cli][slash]" ) {
	auto report = ext::load_report{ };

	auto first = ext::load_outcome{ };
	first.name = "providers";
	first.version = "0.1.0";
	first.description = "Reference model providers";
	first.tools = { "provider_read" };
	first.bytes_used = 320 * 1024;
	report.loaded.push_back( std::move( first ) );

	auto second = ext::load_outcome{ };
	second.name = "skills";
	second.version = "0.2.1";
	second.tools = { "skill_read", "skill_list" };
	second.bytes_used = 512;
	report.loaded.push_back( std::move( second ) );

	report.failed.push_back( { "broken", "/tmp/broken", "unknown key: permission" } );
	report.disabled = 1;

	const auto text = cli::extensions_text( report );

	CHECK( text.find( "2 loaded" ) != std::string::npos );
	CHECK( text.find( "1 failed" ) != std::string::npos );
	CHECK( text.find( "1 disabled" ) != std::string::npos );

	CHECK( text.find( "providers" ) != std::string::npos );
	CHECK( text.find( "skills" ) != std::string::npos );
	CHECK( text.find( "skill_read" ) != std::string::npos );

	// a failure carries its reason, not just its name.
	CHECK( text.find( "broken" ) != std::string::npos );
	CHECK( text.find( "unknown key: permission" ) != std::string::npos );

	CHECK( text.find( "KB" ) != std::string::npos );
}

TEST_CASE( "the extensions command says so when nothing is loaded", "[cli][slash]" ) {
	const auto text = cli::extensions_text( ext::load_report{ } );

	CHECK( text.find( "0 loaded" ) != std::string::npos );
	CHECK( text.find( "none loaded" ) != std::string::npos );

	// A zero failure count is noise on the common path.
	CHECK( text.find( "failed" ) == std::string::npos );
}

TEST_CASE( "the extensions command reports a missing session rather than an empty list",
	"[cli][slash]" ) {
	// Null is "there is no report to read", which is a different answer from
	// "nothing loaded" -- the command is reachable before a session exists.
	auto deps = agent_loop::dependencies{ };
	auto loop = agent_loop{ deps };

	const auto match = cli::match_command( "/extensions", cli::builtin_commands( ) );

	REQUIRE( match.entry != nullptr );

	const auto result = cli::run_command( match, loop, cli::builtin_commands( ), nullptr );

	CHECK( result.output.find( "no session" ) != std::string::npos );
}

TEST_CASE( "the init command asks for a turn instead of writing the file itself",
	"[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-init-generate" );

	auto deps = agent_loop::dependencies{ };
	deps.workspace_root = directory.string( );

	auto loop = agent_loop{ deps };
	const auto match = cli::match_command( "/init", cli::builtin_commands( ) );

	REQUIRE( match.entry != nullptr );

	const auto result = cli::run_command( match, loop, cli::builtin_commands( ) );

	CHECK_FALSE( std::filesystem::exists( directory / "AGENTS.md" ) );

	REQUIRE( result.submit_prompt.has_value( ) );
	CHECK_FALSE( result.submit_prompt->empty( ) );
	CHECK( result.output.empty( ) );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the init command asks to update an existing AGENTS.md rather than refusing",
	"[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-init-existing" );

	std::filesystem::create_directories( directory );

	{
		auto existing = std::ofstream{ directory / "AGENTS.md", std::ios::trunc };
		existing << "# the user's own instructions\n";
	}

	auto deps = agent_loop::dependencies{ };
	deps.workspace_root = directory.string( );

	auto loop = agent_loop{ deps };
	const auto match = cli::match_command( "/init", cli::builtin_commands( ) );

	REQUIRE( match.entry != nullptr );

	const auto result = cli::run_command( match, loop, cli::builtin_commands( ) );

	// The turn is submitted so the model can read the file; the command itself
	// never touches it, which is what keeps the hand-written lines intact.
	REQUIRE( result.submit_prompt.has_value( ) );
	CHECK( result.output.empty( ) );

	{
		auto kept = std::ifstream{ directory / "AGENTS.md" };
		auto line = std::string{ };
		std::getline( kept, line );

		CHECK( line == "# the user's own instructions" );
	}

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the init command submits a different prompt when AGENTS.md is already there",
	"[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-init-prompt" );

	std::filesystem::create_directories( directory );

	auto deps = agent_loop::dependencies{ };
	deps.workspace_root = directory.string( );

	auto loop = agent_loop{ deps };
	const auto match = cli::match_command( "/init", cli::builtin_commands( ) );

	REQUIRE( match.entry != nullptr );

	const auto generated = cli::run_command( match, loop, cli::builtin_commands( ) );

	REQUIRE( generated.submit_prompt.has_value( ) );

	{
		auto existing = std::ofstream{ directory / "AGENTS.md", std::ios::trunc };
		existing << "# the user's own instructions\n";
	}

	const auto updated = cli::run_command( match, loop, cli::builtin_commands( ) );

	REQUIRE( updated.submit_prompt.has_value( ) );
	CHECK( *updated.submit_prompt != *generated.submit_prompt );

	// Only the revise prompt tells the model the file is already on disk.
	CHECK( updated.submit_prompt->find( "already there" ) != std::string::npos );
	CHECK( generated.submit_prompt->find( "already there" ) == std::string::npos );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the resume picker's rows name the command that opens them",
	"[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-session-rows" );

	std::filesystem::create_directories( directory );

	const auto path = directory / "abc123-1791500000000.jsonl";

	{
		auto log = event_log{ };
		REQUIRE( static_cast< bool >( log.open( path ) ) );

		auto message = model::message{ };
		message.speaker = model::role::user;

		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = "add a retry to the uploader\nand keep the backoff";
		message.blocks.push_back( std::move( block ) );

		log.append( std::string{ agent::MESSAGE_USER_EVENT },
			agent::message_to_json( message ) );
		log.close( );
	}

	auto session = cli::session_ref{ };
	session.id = "abc123-1791500000000";
	session.path = path;
	session.started_ms = 1791500000000;
	session.size_bytes = 2048;

	const auto rows = cli::session_rows( { session } );

	REQUIRE( rows.size( ) == 1 );

	// the row's name is the command's own argument, so submitting it runs `/resume <id>`.
	CHECK( rows.front( ).name == "resume abc123-1791500000000" );

	CHECK( rows.front( ).description.find( "add a retry to the uploader" ) != std::string::npos );

	// A newline would break the row it is rendered into.
	CHECK( rows.front( ).description.find( '\n' ) == std::string::npos );
	CHECK( rows.front( ).description.find( "and keep the backoff" ) != std::string::npos );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "a session row without a recorded prompt says so", "[cli][slash]" ) {
	const auto directory = test::scratch_directory( "mcode-session-rows-empty" );

	std::filesystem::create_directories( directory );

	const auto path = directory / "old.jsonl";

	{
		auto log = event_log{ };
		REQUIRE( static_cast< bool >( log.open( path ) ) );
		log.append( "session.start" );
		log.close( );
	}

	auto session = cli::session_ref{ };
	session.id = "old";
	session.path = path;
	session.started_ms = 0;

	const auto rows = cli::session_rows( { session } );

	REQUIRE( rows.size( ) == 1 );
	CHECK( rows.front( ).description.find( "no recorded prompt" ) != std::string::npos );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the session picker filters on a word inside the opening request",
	"[cli][slash]" ) {
	// a session is identified by something remembered from what was asked, so the
	// filter is a substring rather than the ranked prefix match the palette uses.
	const auto rows = std::vector< mcode::tui::slash_command >{
		{ "resume aaa-1", "10:00  2 KB  add a retry to the uploader" },
		{ "resume bbb-2", "11:00  4 KB  fix the dark mode toggle" },
	};

	auto palette = mcode::tui::slash_palette{ };

	cli::refresh_sessions_palette( palette, rows, "" );
	CHECK( palette.matches.size( ) == 2 );

	cli::refresh_sessions_palette( palette, rows, "dark" );
	REQUIRE( palette.matches.size( ) == 1 );
	CHECK( palette.matches.front( ).name == "resume bbb-2" );

	cli::refresh_sessions_palette( palette, rows, "AAA" );
	REQUIRE( palette.matches.size( ) == 1 );
	CHECK( palette.matches.front( ).name == "resume aaa-1" );

	cli::refresh_sessions_palette( palette, rows, "nothing matches this" );
	CHECK( palette.matches.empty( ) );
}

TEST_CASE( "a session row is submitted, not inserted", "[cli][slash]" ) {
	// the row's name is a command argument, so Enter must run it rather than insert it.
	CHECK( cli::submits( cli::palette_source::sessions ) );
	CHECK( cli::submits( cli::palette_source::commands ) );

	CHECK_FALSE( cli::submits( cli::palette_source::mentions ) );
	CHECK_FALSE( cli::submits( cli::palette_source::history ) );

	const auto row = mcode::tui::slash_command{ "resume abc", "" };

	// Tab on a session row still writes the command, so it can be edited.
	CHECK( cli::inserted_line( cli::palette_source::sessions, "", row ) == "/resume abc" );
}

TEST_CASE( "the session commands ask the REPL to act", "[cli][slash]" ) {
	auto deps = agent_loop::dependencies{ };
	auto loop = agent_loop{ deps };
	const auto& builtins = cli::builtin_commands( );

	const auto resume_named = cli::run_command(
		cli::match_command( "/resume abc123", builtins ), loop, builtins );

	REQUIRE( resume_named.adopt_session_id.has_value( ) );
	CHECK( *resume_named.adopt_session_id == "abc123" );
	CHECK_FALSE( resume_named.open_session_picker );

	// `/resume` with no id opens the picker rather than guessing: reopening the
	// newest would silently discard the session the user is in.
	const auto resume_bare = cli::run_command(
		cli::match_command( "/resume", builtins ), loop, builtins );

	CHECK( resume_bare.open_session_picker );
	CHECK_FALSE( resume_bare.adopt_session_id.has_value( ) );

	// an empty id means "newest", which only the session can resolve.
	const auto continued = cli::run_command(
		cli::match_command( "/continue", builtins ), loop, builtins );

	REQUIRE( continued.adopt_session_id.has_value( ) );
	CHECK( continued.adopt_session_id->empty( ) );

	const auto fresh = cli::run_command(
		cli::match_command( "/new", builtins ), loop, builtins );

	CHECK( fresh.start_new_session );
	CHECK_FALSE( fresh.adopt_session_id.has_value( ) );
	CHECK( fresh.output.empty( ) );
}
