#pragma once

#include <string>
#include <vector>

namespace mcode::cli {

	// Inspects the skill roots without starting a session.
	auto run_skill( const std::vector< std::string >& arguments ) -> int;

}
