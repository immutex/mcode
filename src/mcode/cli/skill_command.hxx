#pragma once

#include <string>
#include <vector>

namespace mcode::cli {

	// `mcode skill list|validate` -- inspects the skill roots without starting
	// a session. Returns the process exit code.
	auto run_skill( const std::vector< std::string >& arguments ) -> int;

}
