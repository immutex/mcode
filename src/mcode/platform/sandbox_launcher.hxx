#pragma once

// The POSIX launcher initializer shared by run_process and session::spawn.
// The restriction applies between fork and exec, so the child never runs a
// single instruction unsandboxed. Landlock hard-fails on an unknown ABI and
// Seatbelt hard-fails when the profile is refused; a sandbox that silently
// did not apply is the failure this seam exists to prevent.

#include <filesystem>
#include <system_error>

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
				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			auto ruleset = sandbox_linux_ruleset( *abi, *profile );

			if ( !ruleset ) {
				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			const auto restricted = sandbox_linux_restrict( *abi, *ruleset, *profile );

			if ( !restricted ) {
				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			return { };
#elif defined( __APPLE__ )
			const auto temp = temp_directory( );

			if ( !temp ) {
				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			const auto applied = sandbox_macos_init( *profile, *temp );

			if ( !applied ) {
				return boost::system::error_code{ static_cast< int >( EINVAL ),
					boost::system::system_category( ) };
			}

			return { };
#endif
		}
	};
#endif

}
