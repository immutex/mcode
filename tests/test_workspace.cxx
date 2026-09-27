#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/fs/workspace.hxx"

using mcode::workspace;

namespace {

	struct temp_directory {
		std::filesystem::path path;

		temp_directory( ) {
			path = std::filesystem::temp_directory_path( ) / ( "mcode-test-" + std::to_string( ::rand( ) ) );
			std::filesystem::create_directories( path );
		}

		~temp_directory( ) {
			auto error_code = std::error_code{ };
			std::filesystem::remove_all( path, error_code );
		}

		auto write( const std::string& name, const std::string& content ) const -> void {
			auto out = std::ofstream{ path / name, std::ios::binary };
			out << content;
		}
	};

}

TEST_CASE( "opening a workspace requires an existing directory", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );
	CHECK( opened->root( ) == std::filesystem::weakly_canonical( directory.path ) );

	auto missing = workspace::open( directory.path / "does-not-exist" );
	REQUIRE_FALSE( missing );
	CHECK( missing.error( ).code == mcode::errc::io );
}

TEST_CASE( "relative and absolute paths inside the root resolve", "[workspace]" ) {
	const auto directory = temp_directory{ };
	directory.write( "a.txt", "hello" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto relative = opened->resolve( "a.txt" );
	REQUIRE( relative );
	CHECK( relative->filename( ) == "a.txt" );

	auto absolute = opened->resolve( ( directory.path / "a.txt" ).string( ) );
	REQUIRE( absolute );
	CHECK( *absolute == *relative );

	auto future = opened->resolve( "new-file.txt" );
	REQUIRE( future );
}

TEST_CASE( "paths escaping the root are refused", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto traversal = opened->resolve( "../outside.txt" );
	REQUIRE_FALSE( traversal );
	CHECK( traversal.error( ).code == mcode::errc::io );

	auto deep = opened->resolve( "../../../../../../etc/passwd" );
	REQUIRE_FALSE( deep );

	auto empty = opened->resolve( "" );
	REQUIRE_FALSE( empty );
}

TEST_CASE( "containment is component-wise, not a string prefix", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	CHECK( opened->contains( directory.path / "child" ) );

	const auto sibling = std::filesystem::path{ directory.path.string( ) + "-evil" };
	CHECK_FALSE( opened->contains( sibling ) );
	CHECK_FALSE( opened->resolve( sibling.string( ) ) );
}

TEST_CASE( "glob matches with *, ?, and **", "[workspace]" ) {
	const auto directory = temp_directory{ };
	std::filesystem::create_directories( directory.path / "src" / "deep" );
	directory.write( "top.txt", "t" );
	directory.write( "src/one.cxx", "1" );
	directory.write( "src/two.cxx", "2" );
	directory.write( "src/deep/three.cxx", "3" );
	directory.write( "src/readme.md", "r" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	SECTION( "a single-star pattern stays within one component" ) {
		auto matches = opened->glob( "*.txt" );
		REQUIRE( matches );
		CHECK( matches->size( ) == 1 );
	}

	SECTION( "a recursive pattern descends" ) {
		auto matches = opened->glob( "**/*.cxx" );
		REQUIRE( matches );
		CHECK( matches->size( ) == 3 );
	}

	SECTION( "a directory-scoped pattern does not descend" ) {
		auto matches = opened->glob( "src/*.cxx" );
		REQUIRE( matches );
		CHECK( matches->size( ) == 2 );
	}

	SECTION( "results are sorted for deterministic output" ) {
		auto matches = opened->glob( "**/*.cxx" );
		REQUIRE( matches );
		CHECK( std::is_sorted( matches->begin( ), matches->end( ) ) );
	}

	SECTION( "a question mark matches exactly one character" ) {
		auto matches = opened->glob( "src/???.cxx" );
		REQUIRE( matches );
		CHECK( matches->size( ) == 2 );
	}
}

TEST_CASE( "reading a text file works and reports line numbers", "[workspace]" ) {
	const auto directory = temp_directory{ };
	directory.write( "lines.txt", "alpha\nbeta\ngamma\ndelta\n" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	SECTION( "a full read" ) {
		auto content = opened->read_file( "lines.txt" );
		REQUIRE( content );
		CHECK( content->find( "alpha" ) != std::string::npos );
	}

	SECTION( "a window is 1-based and prefixed with line numbers" ) {
		auto view = opened->read_viewport( "lines.txt", 2, 2 );
		REQUIRE( view );
		CHECK( view->first_line == 2 );
		CHECK( view->last_line == 3 );
		CHECK( view->total_lines == 4 );
		CHECK( view->truncated );
		CHECK( view->text.find( "2\tbeta" ) != std::string::npos );
		CHECK( view->text.find( "3\tgamma" ) != std::string::npos );
		CHECK( view->text.find( "alpha" ) == std::string::npos );
	}

	SECTION( "the last window is not marked truncated" ) {
		auto view = opened->read_viewport( "lines.txt", 3, 10 );
		REQUIRE( view );
		CHECK_FALSE( view->truncated );
	}

	SECTION( "an offset past the end is empty, not an error" ) {
		auto view = opened->read_viewport( "lines.txt", 100, 10 );
		REQUIRE( view );
		CHECK( view->text.empty( ) );
	}

	SECTION( "offset 0 is refused because offsets are 1-based" ) {
		auto view = opened->read_viewport( "lines.txt", 0, 10 );
		REQUIRE_FALSE( view );
	}
}

TEST_CASE( "line counting matches wc -l for trailing newlines", "[workspace]" ) {
	const auto directory = temp_directory{ };
	directory.write( "trailing.txt", "a\nb\nc\n" );
	directory.write( "no-trailing.txt", "a\nb\nc" );
	directory.write( "blank-lines.txt", "a\n\n\nb\n" );
	directory.write( "empty.txt", "" );
	directory.write( "one.txt", "x" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto trailing = opened->read_viewport( "trailing.txt", 1, 100 );
	REQUIRE( trailing );
	CHECK( trailing->total_lines == 3 );
	CHECK_FALSE( trailing->truncated );

	auto no_trailing = opened->read_viewport( "no-trailing.txt", 1, 100 );
	REQUIRE( no_trailing );
	CHECK( no_trailing->total_lines == 3 );

	auto blanks = opened->read_viewport( "blank-lines.txt", 1, 100 );
	REQUIRE( blanks );
	CHECK( blanks->total_lines == 4 );

	auto empty = opened->read_viewport( "empty.txt", 1, 100 );
	REQUIRE( empty );
	CHECK( empty->total_lines == 0 );
	CHECK( empty->text.empty( ) );

	auto one = opened->read_viewport( "one.txt", 1, 100 );
	REQUIRE( one );
	CHECK( one->total_lines == 1 );
	CHECK( one->text.find( "1\tx" ) != std::string::npos );
}

TEST_CASE( "binary files are refused rather than mangled", "[workspace]" ) {
	const auto directory = temp_directory{ };

	{
		auto out = std::ofstream{ directory.path / "blob.bin", std::ios::binary };
		const auto data = std::array< char, 5 >{ 'a', '\0', 'b', '\0', 'c' };
		out.write( data.data( ), data.size( ) );
	}

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto content = opened->read_file( "blob.bin" );
	REQUIRE_FALSE( content );
	CHECK( content.error( ).code == mcode::errc::io );
}

TEST_CASE( "binary detection", "[workspace]" ) {
	CHECK_FALSE( mcode::looks_binary( "plain text\n" ) );
	CHECK_FALSE( mcode::looks_binary( "" ) );
	CHECK( mcode::looks_binary( std::string{ "ab\0cd", 5 } ) );
	CHECK( mcode::looks_binary( "\x01\x02\x03\x04\x05\x06" ) );
}

TEST_CASE( "content hashes are stable and content-sensitive", "[workspace]" ) {
	const auto directory = temp_directory{ };
	directory.write( "a.txt", "same" );
	directory.write( "b.txt", "same" );
	directory.write( "c.txt", "different" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto hash_a = opened->content_hash( "a.txt" );
	auto hash_b = opened->content_hash( "b.txt" );
	auto hash_c = opened->content_hash( "c.txt" );
	REQUIRE( hash_a );
	REQUIRE( hash_b );
	REQUIRE( hash_c );

	CHECK( *hash_a == *hash_b );
	CHECK( *hash_a != *hash_c );
	CHECK( hash_a->size( ) == 16 );
}

TEST_CASE( "display paths use forward slashes on every platform", "[workspace]" ) {
	const auto directory = temp_directory{ };
	std::filesystem::create_directories( directory.path / "a" / "b" );
	directory.write( "a/b/c.txt", "x" );

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto resolved = opened->resolve( "a/b/c.txt" );
	REQUIRE( resolved );
	CHECK( opened->display_path( *resolved ) == "a/b/c.txt" );
}
