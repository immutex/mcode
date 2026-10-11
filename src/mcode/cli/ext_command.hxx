#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mcode::cli {

	// `mcode ext list|trust|deny|install|update|verify` -- inspects and grants
	// extension roots without starting a session. Returns the process exit code.
	auto run_ext( const std::vector< std::string >& arguments ) -> int;

	// Clones, validates, and locks one `git:owner/repo@<ref>` coordinate.
	auto run_ext_install( const std::string& coordinate ) -> int;

}
