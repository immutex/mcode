// The sandbox seam: capability reporting that matches what a probe observes,
// confinement probes on the Windows tier, and the no-orphan guarantee. Split
// from test_config_and_platform.cxx when the seam grew past the honest-stub
// shape.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

namespace {

	auto write_file( const std::filesystem::path& path, const std::string_view text )
		-> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto stream = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		stream << text;
	}

}

TEST_CASE( "the capability pair is internally consistent", "[sandbox]" ) {
	// The two enums must agree: an unavailable filesystem tier cannot claim
	// any network support, and a full tier must claim enforced egress. A pair
	// that contradicts itself is the false claim this seam exists to prevent.
	const auto capability = platform::sandbox_capability_level( );
	const auto network = platform::sandbox_network_level( );

	if ( capability == platform::sandbox_capability::unavailable ) {
		REQUIRE( network == platform::sandbox_network_support::unavailable );
	} else if ( capability == platform::sandbox_capability::full ) {
		REQUIRE( network == platform::sandbox_network_support::enforced );
	} else {
		// write_boundary or filesystem: any network answer is allowed, but it
		// must be one of the three.
		REQUIRE( ( network == platform::sandbox_network_support::unavailable ||
			network == platform::sandbox_network_support::best_effort ||
			network == platform::sandbox_network_support::enforced ) );
	}

	REQUIRE_FALSE( platform::sandbox_mechanism( ).empty( ) );
}

TEST_CASE( "the mechanism string names what is actually enforced", "[sandbox]" ) {
	// The string is the user-facing claim. It must describe the capability the
	// enum reports, so the three places a user can read -- seam, help text,
	// --verbose line -- cannot disagree.
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
	// docs/39 step 7: the help text, the README and the seam must agree --
	// asserted, not reviewed. The help text says the sandbox applies "where
	// the platform supports it", which is only true when the capability is
	// not unavailable.
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
	// The probe, not the return code: apply_sandbox returning success proves
	// nothing. The child attempts the forbidden operation and the result is
	// observed.
	auto root = test::scratch_directory( "mcode-sandbox-write" );
	auto inside = root / "inside";
	auto outside = test::scratch_directory( "mcode-sandbox-outside" );

	std::filesystem::create_directories( inside );

	auto profile = platform::sandbox_profile{ };
	profile.read_paths.push_back( root );
	profile.write_paths.push_back( inside );
	profile.allow_network = false;

	// The deny list is the workspace's own protected subtrees; there are none
	// in this fixture, so the profile denies nothing extra.
	auto options = process_options{ };
	options.executable = "cmd.exe";
	options.args = { "/c", "echo x > outside.txt" };
	options.working_directory = outside.string( );
	options.sandbox = &profile;

	const auto outcome = run_process( options );
	REQUIRE( static_cast< bool >( outcome ) );

	// The child ran. The write it attempted was outside write_paths, and the
	// file must not exist.
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

	// The write inside the profile's write_paths succeeded.
	CHECK( outcome->exit_code == 0 );
	CHECK( std::filesystem::exists( inside / "inside.txt" ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "no sandboxed child survives the harness", "[sandbox]" ) {
	// Kill-on-close is the guarantee: the Job handle is owned by the spawn
	// path and closed when run_process returns, and every process inside the
	// job dies with it. The in-process form of this test is limited -- a
	// child that would outlive the harness only does so when the HARNESS
	// dies first, which an in-process test cannot observe -- so what is
	// asserted here is the observable piece: the child runs inside the Job
	// and is reaped through its Job-assigned process handle.
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

	// The child ran to completion inside the Job and was reaped normally: a
	// real exit code comes back only through the Job-assigned process handle.
	CHECK( outcome->exit_code == 0 );
	CHECK( std::filesystem::exists( root / "marker.txt" ) );

	std::filesystem::remove_all( root );
}

#endif
