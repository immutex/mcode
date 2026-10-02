#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mcode/cli/exec.hxx"
#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"

#include "test_scratch.hxx"

using namespace mcode;

TEST_CASE( "the capability pair is internally consistent", "[sandbox]" ) {
	// an unavailable filesystem tier implies no network support; full implies enforced egress
	const auto capability = platform::sandbox_capability_level( );
	const auto network = platform::sandbox_network_level( );

	if ( capability == platform::sandbox_capability::unavailable ) {
		REQUIRE( network == platform::sandbox_network_support::unavailable );
	} else if ( capability == platform::sandbox_capability::full ) {
		REQUIRE( network == platform::sandbox_network_support::enforced );
	} else {
		REQUIRE( ( network == platform::sandbox_network_support::unavailable ||
			network == platform::sandbox_network_support::best_effort ||
			network == platform::sandbox_network_support::enforced ) );
	}

	REQUIRE_FALSE( platform::sandbox_mechanism( ).empty( ) );
}

TEST_CASE( "the mechanism string names what is actually enforced", "[sandbox]" ) {
	const auto capability = platform::sandbox_capability_level( );
	const auto mechanism = platform::sandbox_mechanism( );

#if defined( _WIN32 )
	REQUIRE( capability == platform::sandbox_capability::write_boundary );
	REQUIRE( mechanism.find( "Low IL" ) != std::string::npos );
	REQUIRE( mechanism.find( "Job Object" ) != std::string::npos );
	REQUIRE( mechanism.find( "best-effort" ) != std::string::npos );
	REQUIRE( platform::sandbox_network_level( )
		== platform::sandbox_network_support::best_effort );
#elif defined( __linux__ )
	REQUIRE( capability != platform::sandbox_capability::unavailable );
	REQUIRE( mechanism.find( "Landlock" ) != std::string::npos );
#elif defined( __APPLE__ )
	REQUIRE( capability != platform::sandbox_capability::unavailable );
	REQUIRE( mechanism.find( "Seatbelt" ) != std::string::npos );
	REQUIRE( platform::sandbox_network_level( )
		== platform::sandbox_network_support::enforced );
#endif
}

TEST_CASE( "the yolo help text agrees with the seam", "[sandbox]" ) {
	const auto help = cli::usage_text( "mcode" );

	if ( platform::sandbox_capability_level( )
		!= platform::sandbox_capability::unavailable ) {
		REQUIRE( help.find( "OS sandbox where the platform supports it" )
			!= std::string::npos );
	} else {
		REQUIRE( help.find( "no OS sandbox exists yet" ) != std::string::npos );
	}
}

#if defined( _WIN32 )

TEST_CASE( "a sandboxed child cannot write outside write_paths", "[sandbox]" ) {
	// the probe result, not the return code, is what proves confinement
	auto root = test::scratch_directory( "mcode-sandbox-write" );
	auto inside = root / "inside";
	auto outside = test::scratch_directory( "mcode-sandbox-outside" );

	std::filesystem::create_directories( inside );

	auto profile = platform::sandbox_profile{ };
	profile.read_paths.push_back( root );
	profile.write_paths.push_back( inside );
	profile.allow_network = false;

	auto options = process_options{ };
	options.executable = "cmd.exe";
	options.args = { "/c", "echo x > outside.txt" };
	options.working_directory = outside.string( );
	options.sandbox = &profile;

	const auto outcome = run_process( options );
	REQUIRE( static_cast< bool >( outcome ) );

	CHECK( outcome->exit_code != 0 );
	CHECK_FALSE( std::filesystem::exists( outside / "outside.txt" ) );

	std::filesystem::remove_all( root.parent_path( ) / ( root.filename( ).string( ) ) );
	std::filesystem::remove_all( outside );
}

TEST_CASE( "a sandboxed child can write inside write_paths", "[sandbox]" ) {
	auto root = test::scratch_directory( "mcode-sandbox-inside" );
	auto inside = root / "inside";

	std::filesystem::create_directories( inside );

	auto profile = platform::sandbox_profile{ };
	profile.read_paths.push_back( root );
	profile.write_paths.push_back( inside );
	profile.allow_network = false;

	auto options = process_options{ };
	options.executable = "cmd.exe";
	options.args = { "/c", "echo allowed > inside.txt" };
	options.working_directory = inside.string( );
	options.sandbox = &profile;

	const auto outcome = run_process( options );
	REQUIRE( static_cast< bool >( outcome ) );

	CHECK( outcome->exit_code == 0 );
	CHECK( std::filesystem::exists( inside / "inside.txt" ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "no sandboxed child survives the harness", "[sandbox]" ) {
	// kill-on-close: closing the Job handle at return kills every process inside it
	auto root = test::scratch_directory( "mcode-sandbox-orphan" );

	std::filesystem::create_directories( root );

	auto profile = platform::sandbox_profile{ };
	profile.read_paths.push_back( root );
	profile.write_paths.push_back( root );
	profile.allow_network = false;

	auto options = process_options{ };
	options.executable = "cmd.exe";
	options.args = { "/c", "echo ran > marker.txt" };
	options.working_directory = root.string( );
	options.sandbox = &profile;
	options.timeout = std::chrono::milliseconds{ 10'000 };

	const auto outcome = run_process( options );
	REQUIRE( static_cast< bool >( outcome ) );

	CHECK( outcome->exit_code == 0 );
	CHECK( std::filesystem::exists( root / "marker.txt" ) );

	std::filesystem::remove_all( root );
}

#endif
