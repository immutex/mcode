#pragma once

#include <string>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// `glob`: path patterns over `workspace::glob`, with the ignore filtering the
	// workspace walk does not do. `.git/` and `.mcode/` are always skipped, and
	// the root `.gitignore` is honoured (simple patterns only; nested files,
	// negation and `**` rules are out of scope for this slice).
	[[nodiscard]] auto handle_glob( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// `grep`: regex content search, ignore-aware, capped during the scan, binary
	// files skipped, one line per match with the match highlighted.
	[[nodiscard]] auto handle_grep( const tool_args& args, tool_context& context )
		-> result< std::string >;

}
