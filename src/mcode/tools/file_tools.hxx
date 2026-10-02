#pragma once

#include <string>
#include <string_view>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// windowed read; records the content hash so a later write or edit is safe
	[[nodiscard]] auto handle_read( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// create needs no prior read; overwrite refuses an unread or stale file
	[[nodiscard]] auto handle_write( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// zero and multiple matches are distinct errors; compares on a normalised copy
	[[nodiscard]] auto handle_edit( const tool_args& args, tool_context& context )
		-> result< std::string >;

}
