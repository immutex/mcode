// The egress half of the sandbox: where the platform can deny network access
// it must, and where it cannot the capability enum must say so rather than
// letting a caller believe the network is closed.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"

#include "test_scratch.hxx"

using namespace mcode;

TEST_CASE( "network denial is reported honestly per platform", "[sandbox][egress]" ) {
	const auto network = platform::sandbox_network_level( );
	const auto capability = platform::sandbox_capability_level( );

	// A platform with no filesystem tier has no egress story either.
	if ( capability == platform::sandbox_capability::unavailable ) {
		REQUIRE( network == platform::sandbox_network_support::unavailable );
		return;
	}

#if defined( _WIN32 )
	// The WFP deny needs an administrator to install; without one the network
	// stays open while the filesystem is confined. best_effort is the honest
	// answer, and the mechanism string says why.
	REQUIRE( network == platform::sandbox_network_support::best_effort );
	REQUIRE( platform::sandbox_mechanism( ).find( "best-effort" ) != std::string::npos );
#elif defined( __linux__ )
	// Landlock's network rules at ABI >= 4, the seccomp filter below that:
	// both are real denials.
	REQUIRE( network == platform::sandbox_network_support::enforced );
#elif defined( __APPLE__ )
	// (deny network*) in the Seatbelt profile.
	REQUIRE( network == platform::sandbox_network_support::enforced );
#endif
}

#if defined( __linux__ ) || defined( __APPLE__ )

TEST_CASE( "allow_network = false blocks egress", "[sandbox][egress]" ) {
	// Only where the platform claims enforced denial can the probe assert
	// that a connection attempt fails. On Windows this test is absent: the
	// capability says best_effort, and a probe asserting a denial would be
	// asserting a claim the platform cannot honour.
	if ( platform::sandbox_network_level( )
		!= platform::sandbox_network_support::enforced ) {
		SUCCEED( "this platform does not claim enforced egress denial" );
		return;
	}

	auto root = test::scratch_directory( "mcode-sandbox-egress" );

	std::filesystem::create_directories( root );

	auto profile = platform::sandbox_profile{ };
	profile.read_paths.push_back( root );
	profile.write_paths.push_back( root );
	profile.allow_network = false;

	// The executable is resolved before the spawn: POSIX `exec` does not search
	// PATH, so a bare "sh" fails with ENOENT even when a shell is present --
	// Windows happens to search, which is why the bare name worked there.
#if defined( _WIN32 )
	const auto shell = find_executable( "cmd.exe" );
#else
	const auto shell = find_executable( "sh" );
#endif
	REQUIRE( static_cast< bool >( shell ) );

	auto options = process_options{ };
	options.executable = *shell;
#if defined( _WIN32 )
	options.args = { "/c", "curl -m 3 -s https://example.invalid > nul" };
#else
	options.args = { "-c", "timeout 3 curl -s https://example.invalid > /dev/null 2>&1" };
#endif
	options.working_directory = root.string( );
	options.sandbox = &profile;
	options.timeout = std::chrono::milliseconds{ 10'000 };

	const auto outcome = run_process( options );

	// The reason matters: a refused profile and a missing binary both arrive
	// as a failed spawn, and they need different fixes.
	if ( !outcome ) {
		FAIL( "the sandboxed spawn failed: " << outcome.error( ).msg );
	}

	// The connection was denied at the OS layer, so the command failed rather
	// than reached the network.
	CHECK( outcome->exit_code != 0 );

	std::filesystem::remove_all( root );
}

#endif
