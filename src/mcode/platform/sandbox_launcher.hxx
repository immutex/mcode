#pragma once

// The POSIX launcher initializer shared by run_process and session::spawn.
// The restriction applies between fork and exec, so the child never runs a
// single instruction unsandboxed. Landlock hard-fails on an unknown ABI and
// Seatbelt hard-fails when the profile is refused; a sandbox that silently
// did not apply is the failure this seam exists to prevent.

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

#if defined( __linux__ ) || defined( __APPLE__ )
#include <unistd.h>
#endif

#include <boost/system/error_code.hpp>

#include "mcode/platform/seams.hxx"

#if defined( __linux__ )
#include "mcode/platform/sandbox_linux.hxx"
#elif defined( __APPLE__ )
#include "mcode/platform/sandbox_macos.hxx"
#endif

#include <boost/process/v2/default_launcher.hpp>

namespace mcode::platform {

#if defined( __linux__ ) || defined( __APPLE__ )

	// The child runs this between fork and exec, so it must not allocate more
	// than it has to and must never throw. It reports through fd 2 directly
	// because the parent's error channel carries only an error_code, and a
	// blind failure here is indistinguishable from a missing binary -- the
	// exact ambiguity that made this seam's first POSIX failure unreadable.
	inline auto report_child_sandbox_failure( const std::string_view what ) -> void {
		const auto* prefix = "mcode: the OS sandbox could not be applied: ";
		const auto prefix_length = std::char_traits< char >::length( prefix );

		if ( ::write( STDERR_FILENO, prefix, prefix_length ) < 0 ) {
			return;
		}

		(void)::write( STDERR_FILENO, what.data( ), what.size( ) );
		(void)::write( STDERR_FILENO, "\n", 1 );
	}

	struct sandbox_posix_initializer {
		const sandbox_profile* profile = nullptr;

		auto on_exec_setup( boost::process::v2::posix::default_launcher& launcher,
			const std::filesystem::path&, const char* const* ) -> boost::system::error_code {
			(void)launcher;

			if ( profile == nullptr ) {
				return { };
			}

#if defined( __linux__ )
			const auto abi = sandbox_linux_abi( );

			if ( !abi ) {
				report_child_sandbox_failure( abi.error( ).msg );

				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			auto ruleset = sandbox_linux_ruleset( *abi, *profile );

			if ( !ruleset ) {
				report_child_sandbox_failure( ruleset.error( ).msg );

				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			const auto restricted = sandbox_linux_restrict( *abi, *ruleset, *profile );

			if ( !restricted ) {
				report_child_sandbox_failure( restricted.error( ).msg );

				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			return { };
#elif defined( __APPLE__ )
			const auto temp = temp_directory( );

			if ( !temp ) {
				report_child_sandbox_failure( "no temp directory could be resolved" );

				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			const auto applied = sandbox_macos_init( *profile, *temp );

			if ( !applied ) {
				report_child_sandbox_failure( applied.error( ).msg );

				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			return { };
#endif
		}
	};
#endif

}
