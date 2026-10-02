#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/instruct/chain.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto write_file( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto out = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		out << text;
	}

	auto options_with( const std::filesystem::path& start ) -> instruct::chain_options {
		auto options = instruct::chain_options{ };
		options.start = start;

		return options;
	}

} // namespace

TEST_CASE( "a root and a nested file concatenate closest last", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-basic" );

	write_file( root / "AGENTS.md", "root rule: tabs\n" );
	write_file( root / "sub" / "AGENTS.md", "nested rule: spaces\n" );

	const auto chain = instruct::assemble_chain( options_with( root / "sub" ) );

	REQUIRE( chain.entries.size( ) == 2 );
	CHECK( chain.entries.front( ).file == root / "AGENTS.md" );
	CHECK( chain.entries.back( ).file == root / "sub" / "AGENTS.md" );
	CHECK( chain.text.find( "root rule: tabs" ) < chain.text.find( "nested rule: spaces" ) );
}

TEST_CASE( "one file per directory, first match wins", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-fallback" );

	write_file( root / "CLAUDE.md", "claude rules\n" );
	write_file( root / "GEMINI.md", "gemini rules\n" );

	const auto chain = instruct::assemble_chain( options_with( root ) );

	REQUIRE( chain.entries.size( ) == 1 );
	CHECK( chain.entries.front( ).file.filename( ) == "CLAUDE.md" );
	CHECK( chain.text.find( "gemini rules" ) == std::string::npos );
}

TEST_CASE( "the walk stops at the repository root", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-repo" );
	const auto inside = root / "repo" / "deep";

	write_file( root / "outside" / "AGENTS.md", "should not be read\n" );
	write_file( root / "repo" / "AGENTS.md", "repo rule\n" );

	std::filesystem::create_directories( root / "repo" / ".git" );

	const auto chain = instruct::assemble_chain( options_with( inside ) );

	REQUIRE( chain.entries.size( ) == 1 );
	CHECK( chain.entries.front( ).file == root / "repo" / "AGENTS.md" );
	CHECK( chain.text.find( "should not be read" ) == std::string::npos );
}

TEST_CASE( "user and org files come before the repository files", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-sources" );
	const auto user = test::scratch_directory( "chain-sources-user" );
	const auto org = test::scratch_directory( "chain-sources-org" );

	write_file( root / "AGENTS.md", "repo rule\n" );
	write_file( user / "AGENTS.md", "user rule\n" );
	write_file( org / "AGENTS.md", "org rule\n" );

	auto options = options_with( root );
	options.user_file = user / "AGENTS.md";
	options.org_file = org / "AGENTS.md";

	const auto chain = instruct::assemble_chain( options );

	REQUIRE( chain.entries.size( ) == 3 );
	CHECK( chain.entries[ 0 ].file == org / "AGENTS.md" );
	CHECK( chain.entries[ 1 ].file == user / "AGENTS.md" );
	CHECK( chain.entries[ 2 ].file == root / "AGENTS.md" );
}

TEST_CASE( "a 40 KiB file is truncated at the file cap with a pointer", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-filecap" );

	write_file( root / "AGENTS.md", std::string( 40u * 1024u, 'a' ) );

	const auto chain = instruct::assemble_chain( options_with( root ) );

	REQUIRE( chain.entries.size( ) == 1 );
	CHECK( chain.entries.front( ).truncated );
	CHECK( chain.text.size( ) < 40u * 1024u );
	CHECK( chain.text.find( "[truncated: read " ) != std::string::npos );
	CHECK( chain.text.find( "AGENTS.md" ) != std::string::npos );
}

TEST_CASE( "a chain over the cap truncates the broadest file, not the closest",
	"[instruct]" ) {
	const auto root = test::scratch_directory( "chain-chaincap" );

	// 24 KiB each: under the file cap, but 72 KiB total exceeds the 64 KiB chain cap.
	write_file( root / "AGENTS.md", std::string( 24u * 1024u, 'r' ) );
	write_file( root / "sub" / "AGENTS.md", std::string( 24u * 1024u, 'm' ) );
	write_file( root / "sub" / "deep" / "AGENTS.md", std::string( 24u * 1024u, 'n' ) );

	const auto chain = instruct::assemble_chain( options_with( root / "sub" / "deep" ) );

	REQUIRE( chain.entries.size( ) == 3 );

	const auto& broadest = chain.entries.front( );
	const auto& closest = chain.entries.back( );

	CHECK( broadest.truncated );
	CHECK( !closest.truncated );
	CHECK( closest.text.size( ) == 24u * 1024u );
	CHECK( chain.text.find( "nnn" ) != std::string::npos );
	// the cap sums entry bytes; the join's newline for each entry is outside it.
	CHECK( chain.text.size( ) <= 64u * 1024u + chain.entries.size( ) );
}

TEST_CASE( "a fat chain is a warning, never a truncation", "[instruct]" ) {
	const auto root = test::scratch_directory( "chain-tokenfat" );

	write_file( root / "AGENTS.md", std::string( 12u * 1024u, 'w' ) );

	const auto chain = instruct::assemble_chain( options_with( root ) );

	CHECK( chain.estimated_tokens > INSTRUCTION_CHAIN_TOKEN_BUDGET );
	CHECK( chain.warnings.size( ) == 1 );
	CHECK( !chain.entries.front( ).truncated );
	// the file has no trailing newline; the assembler adds one.
	CHECK( chain.text.size( ) == 12u * 1024u + 1 );
}

TEST_CASE( "an empty repository produces an empty chain without warnings",
	"[instruct]" ) {
	const auto root = test::scratch_directory( "chain-empty" );

	const auto chain = instruct::assemble_chain( options_with( root ) );

	CHECK( chain.entries.empty( ) );
	CHECK( chain.text.empty( ) );
	CHECK( chain.warnings.empty( ) );
	CHECK( chain.estimated_tokens == 0 );
}

TEST_CASE( "the prompt states closest-wins and orders the contradiction correctly",
	"[instruct]" ) {
	const auto root = test::scratch_directory( "chain-prompt" );

	write_file( root / "AGENTS.md", "indent with spaces\n" );
	write_file( root / "sub" / "AGENTS.md", "indent with tabs\n" );

	auto registry = tool_registry{ };

	const auto prompt = build_system_prompt( registry,
		instruct::assemble_chain( options_with( root / "sub" ) ).text, { } );

	const auto rule = prompt.find( "CLOSEST" );

	CHECK( rule != std::string::npos );
	CHECK( prompt.find( "indent with spaces" ) < prompt.find( "indent with tabs" ) );
}
