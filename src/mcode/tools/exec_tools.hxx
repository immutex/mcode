#pragma once

#include <string>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// a non-zero exit is a successful call whose content says so; never retries
	[[nodiscard]] auto handle_bash( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// headless: fails with the question in the error instead of prompting
	[[nodiscard]] auto handle_ask_user( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// two-stage disclosure: name + one-line description, full schema on expand
	[[nodiscard]] auto handle_tool_search( const tool_args& args, tool_context& context,
		const class tool_registry& registry ) -> result< std::string >;

}
