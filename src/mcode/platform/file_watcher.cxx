#include "mcode/platform/seams.hxx"

#include <filesystem>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::platform {

	auto file_watcher::supported( ) noexcept -> bool {
		return false;
	}

	auto file_watcher::create( const std::filesystem::path&, const bool )
		-> result< file_watcher > {
		return std::unexpected( fail( errc::unsupported,
			"file watching is a Lua extension over the core API, not a core primitive" ) );
	}

	auto file_watcher::poll( ) -> std::vector< file_event > {
		return { };
	}

}
