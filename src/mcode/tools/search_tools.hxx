#pragma once

#include <string>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// .git/ and .mcode/ are always skipped; only the root .gitignore is honoured
	[[nodiscard]] auto handle_glob( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// ignore-aware, capped during the scan, binary files skipped
	[[nodiscard]] auto handle_grep( const tool_args& args, tool_context& context )
		-> result< std::string >;

}
