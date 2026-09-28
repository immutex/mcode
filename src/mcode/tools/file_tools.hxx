#pragma once

#include <string>
#include <string_view>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// The three file tools. All of them enforce the read-before-write invariant
	// through `session_reads`, refuse the deny-write paths via
	// `workspace::is_protected`, and go through `workspace::resolve` so the
	// workspace boundary is the only path to the filesystem.

	// `read`: windowed file read with 1-based line numbers. Refuses binary with a
	// stub, serves a window from files past the whole-file read cap, suggests the
	// closest existing path on a miss, and records the content hash so a later
	// write or edit is safe.
	[[nodiscard]] auto handle_read( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// `write`: create or overwrite. Create needs no prior read; overwrite refuses
	// a file not read this session, and one whose hash moved on disk (stale).
	[[nodiscard]] auto handle_write( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// `edit`: exact-anchor replacement returning a unified diff. Zero and
	// multiple matches are distinct errors. Preserves the file's line endings;
	// compares on a normalised copy.
	[[nodiscard]] auto handle_edit( const tool_args& args, tool_context& context )
		-> result< std::string >;

}
