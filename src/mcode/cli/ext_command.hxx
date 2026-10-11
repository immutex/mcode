#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mcode::cli {

	// Inspects and grants extension roots without starting a session; returns the process
	// exit code.
	auto run_ext( const std::vector< std::string >& arguments ) -> int;

	auto run_ext_install( const std::string& coordinate ) -> int;

}
