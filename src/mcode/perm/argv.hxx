#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/registry.hxx"

namespace mcode::perm {

	// metacharacters make the line unparsable, and the caller must then refuse.
	[[nodiscard]] auto parse_command_line( std::string_view command )
		-> std::optional< std::vector< std::string > >;

	[[nodiscard]] auto is_exec_runner( std::string_view program ) noexcept -> bool;

}

