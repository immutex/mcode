#pragma once

#include <filesystem>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode::platform {

	[[nodiscard]] auto sandbox_macos_init( const sandbox_profile& profile,
		const std::filesystem::path& temp_dir ) -> status;

}
