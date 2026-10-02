#pragma once

#include <memory>
#include <filesystem>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

#if defined( __linux__ )
#include <unistd.h>
#endif

namespace mcode::platform {

#if defined( __linux__ )

	// the ruleset fd from landlock_create_ruleset; restrict_self consumes it.
	class unique_ruleset_linux {
	public:
		unique_ruleset_linux( ) = default;

		explicit unique_ruleset_linux( const int descriptor ) noexcept
			: descriptor_( descriptor ) { }

		~unique_ruleset_linux( ) {
			if ( descriptor_ >= 0 ) {
				::close( descriptor_ );
			}
		}

		unique_ruleset_linux( const unique_ruleset_linux& ) = delete;
		auto operator=( const unique_ruleset_linux& ) -> unique_ruleset_linux& = delete;

		unique_ruleset_linux( unique_ruleset_linux&& other ) noexcept
			: descriptor_( other.descriptor_ ) {
			other.descriptor_ = -1;
		}

		auto operator=( unique_ruleset_linux&& other ) noexcept -> unique_ruleset_linux& {
			if ( this != &other ) {
				if ( descriptor_ >= 0 ) {
					::close( descriptor_ );
				}

				descriptor_ = other.descriptor_;
				other.descriptor_ = -1;
			}

			return *this;
		}

		[[nodiscard]] auto get( ) const noexcept -> int { return descriptor_; }

	private:
		int descriptor_ = -1;
	};

	// newer ABIs are supersets; an unknown one hard-fails rather than risk a partial ruleset.
	inline constexpr int LANDLOCK_ABI_MAX = 5;

	// below this ABI the seccomp filter is the network deny.
	inline constexpr int LANDLOCK_NETWORK_ABI = 4;
#endif

	// parses on other platforms; never constructed there.
#if !defined( __linux__ )
	struct unique_ruleset_linux_placeholder { };
	using unique_ruleset_linux = unique_ruleset_linux_placeholder;
#endif

	// the runtime probe: a 5.14 kernel can report ABI 5, so no compile-time assumption holds.
	[[nodiscard]] auto sandbox_linux_abi( ) -> result< int >;

	[[nodiscard]] auto sandbox_linux_ruleset( const int abi,
		const sandbox_profile& profile ) -> result< unique_ruleset_linux >;

	// sets no_new_privs before restrict_self, then the seccomp deny when the ABI lacks net rules.
	[[nodiscard]] auto sandbox_linux_restrict( const int abi,
		const unique_ruleset_linux& ruleset, const sandbox_profile& profile ) -> status;

	// denies socket, connect and socketpair with EPERM, for ABIs below 4.
	[[nodiscard]] auto sandbox_linux_seccomp_deny_network( ) -> status;

}
