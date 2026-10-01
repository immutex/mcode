// The file-watcher seam. Split from seams.cxx, which owns the platform
// queries; this one has no platform branch at all.
#include "mcode/platform/seams.hxx"

#include <filesystem>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::platform {

	auto file_watcher::supported( ) noexcept -> bool {
		// The interface exists so the extension API has a shape to target; the real
		// implementation is a Lua extension over a small core API.
		return false;
	}

	auto file_watcher::create( const std::filesystem::path&, const bool ) -> result< file_watcher > {
		return std::unexpected( fail( errc::unsupported,
			"file watching is a Lua extension over the core API, not a core primitive (docs/24)" ) );
	}

	auto file_watcher::poll( ) -> std::vector< file_event > {
		return { };
	}

}
