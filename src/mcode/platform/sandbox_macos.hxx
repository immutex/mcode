#pragma once

// macOS half of the Sandbox seam: Seatbelt via sandbox_init_with_parameters,
// deny-default only. The profile is a small static shape with the workspace
// paths substituted; it is never generated from user input beyond those paths.

#include <filesystem>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode::platform {

	// Applies the profile to the calling process in-process. The profile is
	// (deny default) + (import "system.sb") plus the read/write grants the
	// profile names; (allow default) is structurally escapable and never
	// emitted. sandbox-exec stays a documented fallback, not this path.
	[[nodiscard]] auto sandbox_macos_init( const sandbox_profile& profile,
		const std::filesystem::path& temp_dir ) -> status;

}
