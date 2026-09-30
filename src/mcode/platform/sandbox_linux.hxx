#pragma once

// Linux half of the Sandbox seam: Landlock with mandatory runtime ABI
// detection, plus a seccomp fallback for the network denials older ABIs
// cannot express. The unique_ruleset_linux type owns the ruleset fd.

#include <memory>
#include <filesystem>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

#if defined( __linux__ )
#include <unistd.h>
#endif

namespace mcode::platform {

#if defined( __linux__ )

	struct ruleset_closer {
		auto operator( )( int value ) const -> void {
			if ( value >= 0 ) {
				::close( value );
			}
		}
	};

	// The ruleset fd from landlock_create_ruleset. Rules are added while it is
	// open; restrict_self consumes it.
	using unique_ruleset_linux = std::unique_ptr< int, ruleset_closer >;

	// The ABI ladder this code knows. A kernel reporting a version above
	// LANDLOCK_ABI_MAX is handled: newer ABIs are supersets, so the newest
	// known feature set is applied. The cost of that choice is real and
	// accepted here: a future ABI that ADDS a filesystem right this code does
	// not know leaves that right unhandled, and unhandled means NOT
	// RESTRICTED -- the sandbox is complete for every right the code knows
	// and silently permissive for any it does not. A kernel reporting a
	// version the code has never seen is NOT assumed -- it hard-fails, because
	// "never seen" is exactly what a partial ruleset looks like from inside.
	inline constexpr int LANDLOCK_ABI_MAX = 5;

	// The first ABI that can express the network rights (NET_BIND_TCP /
	// NET_CONNECT_TCP). Below it the seccomp filter is the network deny.
	inline constexpr int LANDLOCK_NETWORK_ABI = 4;
#endif

	// A placeholder on other platforms so the declarations below parse; the
	// definitions are compiled only on Linux and the placeholder is never
	// constructed anywhere.
#if !defined( __linux__ )
	struct unique_ruleset_linux_placeholder { };
	using unique_ruleset_linux = unique_ruleset_linux_placeholder;
#endif

	// Queries the kernel's Landlock ABI version. This is the mandatory probe:
	// RHEL 9.6 reports ABI 5 on a 5.14 kernel, so no compile-time assumption
	// about the header's feature set can be trusted.
	[[nodiscard]] auto sandbox_linux_abi( ) -> result< int >;

	// Builds a ruleset for the profile at the given ABI. Read paths get
	// read+execute rights, write paths get the full known right set. When
	// allow_network is false and the ABI supports it, the net rights are
	// handled (and therefore denied, since no port rule grants them).
	[[nodiscard]] auto sandbox_linux_ruleset( const int abi,
		const sandbox_profile& profile ) -> result< unique_ruleset_linux >;

	// Applies the ruleset to the calling process: sets no_new_privs first,
	// then restrict_self, then the seccomp network deny when the ABI could not
	// express it.
	[[nodiscard]] auto sandbox_linux_restrict( const int abi,
		const unique_ruleset_linux& ruleset, const sandbox_profile& profile ) -> status;

	// The seccomp fallback: a filter denying socket, connect and socketpair
	// with EPERM. Used when the kernel's Landlock ABI predates network rules.
	[[nodiscard]] auto sandbox_linux_seccomp_deny_network( ) -> status;

}
