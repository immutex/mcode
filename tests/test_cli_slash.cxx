#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/cli/slash.hxx"
#include "mcode/model/types.hxx"
#include "mcode/tui/mention.hxx"
#include "mcode/tui/palette.hxx"

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

	auto files( ) -> const std::vector< std::string >& {
		static const auto LIST = std::vector< std::string >{
			"src/main.cxx",
			"src/model/client.cxx",
			"docs/notes.md",
		};

		return LIST;
	}

}

TEST_CASE( "an empty query lists every command in registration order",
	"[cli][palette]" ) {
	const auto matches = tui::filter_commands( commands( ), "" );

	REQUIRE( matches.size( ) == 4 );
	CHECK( matches[ 0 ].name == "help" );
	CHECK( matches[ 1 ].name == "model" );
	CHECK( matches[ 2 ].name == "tools" );
	CHECK( matches[ 3 ].name == "remodel" );
}

TEST_CASE( "a prefix match outranks a command that merely contains the query",
	"[cli][palette]" ) {
	const auto matches = tui::filter_commands( commands( ), "mo" );

	REQUIRE( matches.size( ) == 2 );
	CHECK( matches[ 0 ].name == "model" );
	CHECK( matches[ 1 ].name == "remodel" );
}

TEST_CASE( "a query matching nothing yields an empty list", "[cli][palette]" ) {
	CHECK( tui::filter_commands( commands( ), "zzz" ).empty( ) );
}

TEST_CASE( "a bare slash is a command query, a path is not", "[cli][palette]" ) {
	const auto bare = tui::command_query( "/" );

	REQUIRE( bare.has_value( ) );
	CHECK( bare->empty( ) );

	const auto partial = tui::command_query( "/he" );

	REQUIRE( partial.has_value( ) );
	CHECK( *partial == "he" );
}

TEST_CASE( "a space, a second slash, or no slash ends the command query",
	"[cli][palette]" ) {
	CHECK_FALSE( tui::command_query( "/help " ).has_value( ) );
	CHECK_FALSE( tui::command_query( "/tmp/notes" ).has_value( ) );
	CHECK_FALSE( tui::command_query( "notacommand" ).has_value( ) );
	CHECK_FALSE( tui::command_query( "" ).has_value( ) );
}

TEST_CASE( "completing a name leaves room for arguments", "[cli][palette]" ) {
	CHECK( tui::completed_command( "help" ) == "/help " );
}

TEST_CASE( "the palette opens on a command query and closes when it is not one",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );

	CHECK( palette.open );
	CHECK( palette.matches.size( ) == 4 );

	cli::refresh_palette( palette, commands( ), "/mo" );

	CHECK( palette.open );
	REQUIRE( palette.matches.size( ) == 2 );
	CHECK( palette.matches.front( ).name == "model" );

	cli::refresh_palette( palette, commands( ), "hello" );

	CHECK_FALSE( palette.open );
	CHECK( palette.matches.empty( ) );
}

TEST_CASE( "a selection past the end of a narrowed list resets to the top",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );
	palette.selected = 2;

	cli::refresh_palette( palette, commands( ), "/mo" );

	REQUIRE( palette.matches.size( ) == 2 );
	CHECK( palette.selected == 0 );
}

TEST_CASE( "Enter on a highlighted row runs that row's command, not the typed text",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );

	REQUIRE( palette.matches.size( ) == 4 );
	CHECK( tui::submitted_line( palette, "/" ) == "/help" );

	// The reproduced defect: arrow down to the second row, then Enter. The
	// submitted line is the selection, never the raw "/".
	palette.selected = 1;
	CHECK( tui::submitted_line( palette, "/" ) == "/model" );

	// The typed text is echoed through when no row is highlighted, so a
	// filtered-to-nothing palette still submits what the user wrote.
	palette.matches.clear( );
	CHECK( tui::submitted_line( palette, "/zzz" ) == "/zzz" );

	auto closed = tui::slash_palette{ };
	CHECK( tui::submitted_line( closed, "/model extra" ) == "/model extra" );
}

TEST_CASE( "Tab inserts the completed command and leaves room for arguments",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/mo" );

	const auto* row = palette.highlighted( );

	REQUIRE( row != nullptr );
	CHECK( tui::completed_command( row->name ) == "/model " );
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

TEST_CASE( "a mention starts a word and closes on a space", "[cli][mention]" ) {
	const auto query = tui::mention_query( "see @src/f" );

	REQUIRE( query.has_value( ) );
	CHECK( *query == "src/f" );

	CHECK_FALSE( tui::mention_query( "a@b" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "@x y" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "no mention here" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "" ).has_value( ) );
}

TEST_CASE( "completing a mention inserts the path, never the contents",
	"[cli][mention]" ) {
	CHECK( tui::mention_completion( "see @src/f", "src/foo.cxx" ) == "see @src/foo.cxx " );
	CHECK( tui::mention_completion( "plain text", "src/foo.cxx" ) == "plain text" );
}

TEST_CASE( "a prefix match outranks a subsequence match", "[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "src/m", 8 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "src/main.cxx" );
	CHECK( ranked[ 1 ] == "src/model/client.cxx" );
}

TEST_CASE( "a basename prefix outranks a mid-path subsequence", "[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "client", 8 );

	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "src/model/client.cxx" );
}

TEST_CASE( "an empty query keeps the input order and honours the cap",
	"[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "", 2 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "src/main.cxx" );
	CHECK( ranked[ 1 ] == "src/model/client.cxx" );
}

TEST_CASE( "a query matching no path yields nothing", "[cli][mention]" ) {
	CHECK( tui::rank_mentions( files( ), "zzzzz", 8 ).empty( ) );
}

TEST_CASE( "the mention picker fills the same palette with a cleared prefix",
	"[cli][mention]" ) {
	auto palette = tui::slash_palette{ };
	auto index = tui::mention_index{ nullptr };

	cli::refresh_mention_palette( palette, index, "@" );

	// No workspace is a picker with no files, not an error.
	CHECK( palette.open );
	CHECK( palette.empty( ) );
	CHECK( palette.prefix.empty( ) );

	cli::refresh_mention_palette( palette, index, "hello" );

	CHECK_FALSE( palette.open );
}

namespace {

	auto submitted( ) -> const std::vector< std::string >& {
		static const auto LIST = std::vector< std::string >{
			"run the tests",
			"fix the parser",
			"run the linter",
		};

		return LIST;
	}

}

TEST_CASE( "an empty history query is the most recent entries, newest first",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "", 8 );

	REQUIRE( ranked.size( ) == 3 );
	CHECK( ranked[ 0 ] == "run the linter" );
	CHECK( ranked[ 1 ] == "fix the parser" );
	CHECK( ranked[ 2 ] == "run the tests" );
}

TEST_CASE( "a history prefix match outranks a later substring match",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "run", 8 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "run the linter" );
	CHECK( ranked[ 1 ] == "run the tests" );
}

TEST_CASE( "a history query that is only a substring still matches",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "parser", 8 );

	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "fix the parser" );
}

TEST_CASE( "a history query matching nothing yields nothing", "[cli][history]" ) {
	CHECK( cli::rank_history( submitted( ), "zzzzz", 8 ).empty( ) );
}

TEST_CASE( "a history query that is only a subsequence still matches, last",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "rtl", 8 );

	// "run the linter" is a subsequence; the others are not.
	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "run the linter" );
}

TEST_CASE( "the history palette opens on Ctrl+R and inserts rather than submits",
	"[cli][history]" ) {
	auto palette = tui::slash_palette{ };
	auto controller = cli::palette_controller{ { &commands( ), nullptr, &submitted( ) } };

	CHECK_FALSE( cli::submits( cli::palette_source::history ) );
	CHECK_FALSE( cli::submits( cli::palette_source::mentions ) );
	CHECK( cli::submits( cli::palette_source::commands ) );

	controller.open_history( palette, "" );

	CHECK( palette.open );
	CHECK( controller.source( ) == cli::palette_source::history );
	CHECK( palette.prefix.empty( ) );
	REQUIRE( palette.matches.size( ) == 3 );
	CHECK( palette.matches[ 0 ].name == "run the linter" );

	// Typing filters the search rather than the prompt.
	controller.refresh( palette, "parser" );

	REQUIRE( palette.matches.size( ) == 1 );
	CHECK( palette.matches[ 0 ].name == "fix the parser" );

	// Enter on that row inserts it, and the source goes back to commands.
	const auto* row = palette.highlighted( );

	REQUIRE( row != nullptr );
	CHECK( cli::inserted_line( controller.source( ), "parser", *row ) == "fix the parser" );

	controller.reset( );
	CHECK( controller.source( ) == cli::palette_source::commands );

	controller.refresh( palette, "parser" );
	CHECK_FALSE( palette.open );
}

TEST_CASE( "the command source is what a bare prompt implies",
	"[cli][history]" ) {
	auto palette = tui::slash_palette{ };
	auto controller = cli::palette_controller{ { &commands( ), nullptr, &submitted( ) } };

	controller.refresh( palette, "/" );

	CHECK( controller.source( ) == cli::palette_source::commands );
	CHECK( palette.prefix == "/" );
	CHECK( palette.matches.size( ) == 4 );

	// A non-command prompt closes the palette and stays on the commands.
	controller.refresh( palette, "plain text" );

	CHECK_FALSE( palette.open );
	CHECK( controller.source( ) == cli::palette_source::commands );
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

	// The counts, because that is the first thing a reader looks for.
	CHECK( text.find( "2 loaded" ) != std::string::npos );
	CHECK( text.find( "1 failed" ) != std::string::npos );
	CHECK( text.find( "1 disabled" ) != std::string::npos );

	// Every loaded name, and each one's tools.
	CHECK( text.find( "providers" ) != std::string::npos );
	CHECK( text.find( "skills" ) != std::string::npos );
	CHECK( text.find( "skill_read" ) != std::string::npos );

	// A failure carries its reason, not just its name: the reason names the key
	// or the permission that was refused, which is the whole point of asking.
	CHECK( text.find( "broken" ) != std::string::npos );
	CHECK( text.find( "unknown key: permission" ) != std::string::npos );

	// The memory figure is a tracked budget (`docs/28` measured ~320 KB per VM),
	// so it is reported rather than hidden.
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

	// The command must not write: the file is generated from what is actually in
	// the repository, so the model explores and writes it. A command that wrote
	// its own file here would be the scaffold this replaced.
	CHECK_FALSE( std::filesystem::exists( directory / "AGENTS.md" ) );

	REQUIRE( result.submit_prompt.has_value( ) );
	CHECK_FALSE( result.submit_prompt->empty( ) );
	CHECK( result.output.empty( ) );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "the init command refuses to regenerate over an existing AGENTS.md", "[cli][slash]" ) {
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

	// No turn is submitted, so no model call is spent on a file that will not be
	// written, and the message says how to proceed deliberately.
	CHECK_FALSE( result.submit_prompt.has_value( ) );
	CHECK( result.output.find( "already exists" ) != std::string::npos );

	// The user's file is untouched -- the point of refusing.
	{
		auto kept = std::ifstream{ directory / "AGENTS.md" };
		auto line = std::string{ };
		std::getline( kept, line );

		CHECK( line == "# the user's own instructions" );
	}

	std::filesystem::remove_all( directory );
}
