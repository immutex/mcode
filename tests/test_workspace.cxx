#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#if defined( _WIN32 )
#include <windows.h>
#endif

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"

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

#if defined( _WIN32 )
	// The 8.3 form of a path. CI runs under a temp directory whose spelling is
	// `RUNNER~1`, and that is the exact input that broke `contains`.
	auto short_path( const std::filesystem::path& path ) -> std::filesystem::path {
		auto buffer = std::array< wchar_t, 1024 >{ };
		const auto length = ::GetShortPathNameW( path.c_str( ), buffer.data( ),
			static_cast< DWORD >( buffer.size( ) ) );

		if ( length == 0 || length >= buffer.size( ) ) {
			return { };
		}

		return std::filesystem::path{ buffer.data( ) };
	}
#endif

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

#if defined( _WIN32 )
TEST_CASE( "contains accepts the 8.3 spelling of the same directory", "[workspace]" ) {
	// Windows spells one directory two ways, and `%TEMP%` on a CI runner is the
	// short form (`RUNNER~1`). The canonical root is the long form, so a
	// component-wise comparison against a raw short path fails on a directory that
	// is plainly inside the workspace -- which is how this test was found.
	//
	// A long leaf name is required: only components over eight characters get a
	// short form at all.
	const auto directory = temp_directory{ };

	const auto short_root = short_path( directory.path );

	if ( short_root.empty( ) || short_root == directory.path ) {
		// Short names can be disabled on a volume; the case cannot be constructed,
		// so there is nothing to assert.
		SUCCEED( "short names unavailable on this volume" );

		return;
	}

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	// Both spellings resolve to the same directory and both must be accepted.
	CHECK( opened->contains( directory.path / "child" ) );
	CHECK( opened->contains( short_root / "child" ) );

	// And the guarantee the function exists for still holds for the short form:
	// a sibling that merely shares a string prefix is not contained.
	const auto sibling = std::filesystem::path{ short_root.string( ) + "-evil" };
	CHECK_FALSE( opened->contains( sibling ) );
}
#endif

TEST_CASE( "a directory symlink loop does not hang the glob", "[workspace]" ) {
	// `**` recursed into every entry whose is_directory() was true, and
	// is_directory FOLLOWS a symlink -- so a clone containing a -> b -> a made the
	// walk recurse until the stack ran out. The result cap did not help: it counts
	// matches, not visits, so a tree with few matching files never hit it.
	auto root = std::filesystem::temp_directory_path( ) / "mcode-glob-loop-test";
	std::filesystem::remove_all( root );
	std::filesystem::create_directories( root / "a" / "b" );

	auto opened = workspace::open( root );
	REQUIRE( static_cast< bool >( opened ) );

	if ( !opened ) {
		FAIL( opened.error( ).msg );
	}

	// b/loop -> a, which closes the cycle.
	auto error = std::error_code{ };
	std::filesystem::create_directory_symlink( root / "a", root / "a" / "b" / "loop", error );

	if ( error ) {
		// Symlinks need a privilege this process may not have. The test is then
		// vacuous, so it says so rather than passing silently.
		WARN( "skipped: cannot create a symlink here (" << error.message( ) << ")" );
		std::filesystem::remove_all( root );

		return;
	}

	// The walk must terminate. Depth and visited-set guards are what make this
	// finish; without them it is a stack overflow, not a slow answer.
	auto matched = opened->glob( "**/*.txt" );

	REQUIRE( static_cast< bool >( matched ) );

	if ( matched ) {
		// And nothing it returned came from outside the root.
		for ( const auto& path : *matched ) {
			REQUIRE( opened->contains( path ) );
		}
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "a path past MAX_PATH is read, not refused", "[workspace]" ) {
	// Windows caps a path at 260 characters unless it carries the \\?\ prefix or
	// the machine sets LongPathsEnabled -- and that value defaults to 0. A deep
	// cloned repository therefore failed to resolve on a stock machine, which the
	// platform seam exists to prevent.
	const auto root = std::filesystem::temp_directory_path( ) / "mcode-longpath-test";

	// remove_all needs the extended form too, or the cleanup fails for the very
	// reason this test is about and the failure looks like the test's own bug.
	std::filesystem::remove_all( mcode::platform::to_extended_path( root ) );
	std::filesystem::create_directories( root );

	auto deep = root;
	const auto segment = std::string( 20, 'a' );

	// Deep enough that the file's full path exceeds 260 characters on any platform
	// with a non-trivial temp root.
	for ( auto level = 0; level < 14; ++level ) {
		deep /= segment;
	}

	auto opened = workspace::open( root );
	REQUIRE( static_cast< bool >( opened ) );

	if ( !opened ) {
		FAIL( opened.error( ).msg );
	}

	// Create the tree with the extended form, so the SETUP is not what fails.
	auto error = std::error_code{ };
	std::filesystem::create_directories( mcode::platform::to_extended_path( deep ), error );

	if ( error ) {
		WARN( "skipped: cannot create a deep tree (" << error.message( ) << ")" );
		std::filesystem::remove_all( mcode::platform::to_extended_path( root ) );

		return;
	}

	const auto file = deep / "deep.txt";
	{
		auto out = std::ofstream{ mcode::platform::to_extended_path( file ), std::ios::trunc };
		out << "deep content\n";
	}

	REQUIRE( file.string( ).size( ) > 260 );

	// Resolve through the workspace, which is where the seam is applied.
	auto relative = std::filesystem::relative( file, root, error );
	REQUIRE( !error );

	auto resolved = opened->resolve( relative.string( ) );
	REQUIRE( static_cast< bool >( resolved ) );

	if ( !resolved ) {
		FAIL( resolved.error( ).msg );
	}

	// And read it, which is the syscall boundary the prefix is applied at.
	auto content = opened->read_file( relative.string( ) );
	REQUIRE( static_cast< bool >( content ) );

	if ( content ) {
		REQUIRE( content->find( "deep content" ) != std::string::npos );
	}

	std::filesystem::remove_all( mcode::platform::to_extended_path( root ) );
}

TEST_CASE( "write creates and overwrites with the right mode", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	// create on a path that is absent succeeds, and reports the hash of what it
	// wrote so the caller can record it.
	auto created = opened->write_file( "notes.txt", "first\n", mcode::write_mode::create );
	REQUIRE( static_cast< bool >( created ) );

	if ( created ) {
		REQUIRE( created->bytes_written == 6 );
		REQUIRE_FALSE( created->content_hash.empty( ) );
		REQUIRE( created->content_hash == mcode::hash_bytes( "first\n" ) );
	}

	// create on an existing path is a different error from overwrite on a missing
	// one, because the caller acts differently on each.
	auto again = opened->write_file( "notes.txt", "second\n", mcode::write_mode::create );
	REQUIRE_FALSE( again );
	CHECK( again.error( ).msg.find( "already exists" ) != std::string::npos );

	auto missing = opened->write_file( "absent.txt", "x", mcode::write_mode::overwrite );
	REQUIRE_FALSE( missing );
	CHECK( missing.error( ).msg.find( "does not exist" ) != std::string::npos );

	// overwrite replaces the content and the hash moves with it.
	auto replaced = opened->write_file( "notes.txt", "second\n", mcode::write_mode::overwrite );
	REQUIRE( static_cast< bool >( replaced ) );

	if ( replaced ) {
		REQUIRE( replaced->content_hash != created->content_hash );
	}

	auto content = opened->read_file( "notes.txt" );
	REQUIRE( static_cast< bool >( content ) );

	if ( content ) {
		REQUIRE( *content == "second\n" );
	}

	// No temp file is left behind, which is the observable part of the atomic
	// write.
	auto leftovers = std::size_t{ 0 };

	for ( const auto& entry : std::filesystem::directory_iterator{ directory.path } ) {
		if ( entry.path( ).filename( ).string( ).find( "mcode-tmp-" ) != std::string::npos ) {
			++leftovers;
		}
	}

	CHECK( leftovers == 0 );
}

TEST_CASE( "write refuses paths outside the root and oversized content", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	auto escape = opened->write_file( "../outside.txt", "x", mcode::write_mode::create );
	REQUIRE_FALSE( escape );
	CHECK( escape.error( ).code == mcode::errc::io );

	// One byte over the cap. The cap is a named constant, so the test reads it
	// rather than restating the number.
	const auto oversized = std::string( mcode::MAX_WRITE_FILE_BYTES + 1, 'a' );

	auto too_big = opened->write_file( "big.txt", oversized, mcode::write_mode::create );
	REQUIRE_FALSE( too_big );
	CHECK( too_big.error( ).msg.find( "write cap" ) != std::string::npos );

	CHECK_FALSE( std::filesystem::exists( directory.path / "big.txt" ) );
}

TEST_CASE( "protected paths are .mcode and .git at the root only", "[workspace]" ) {
	const auto directory = temp_directory{ };

	auto opened = workspace::open( directory.path );
	REQUIRE( opened );

	// The primitive does not enforce this -- the harness writes artifacts under
	// .mcode/artifacts/ -- so the query is what the tool layer consults.
	CHECK( opened->is_protected( directory.path / ".mcode" / "config.toml" ) );
	CHECK( opened->is_protected( directory.path / ".git" / "hooks" / "pre-commit" ) );
	CHECK( opened->is_protected( directory.path / ".mcode" / "artifacts" / "run-1" / "out.txt" ) );

	// A name that merely starts with the same characters is not protected, which is
	// the case a prefix check gets wrong.
	CHECK_FALSE( opened->is_protected( directory.path / "notes.mcode" ) );
	CHECK_FALSE( opened->is_protected( directory.path / "src" / ".mcode" / "x" ) );
	CHECK_FALSE( opened->is_protected( directory.path / "src" / "main.cxx" ) );

	// And the write itself still succeeds there, which is what the harness needs.
	auto artifact = opened->write_file( ".mcode/artifacts/run-1/out.txt", "spilled\n",
		mcode::write_mode::create );

	REQUIRE( static_cast< bool >( artifact ) );
}
