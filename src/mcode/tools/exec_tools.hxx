#pragma once

#include <string>

#include "mcode/tools/context.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::tools {

	// `bash`: exec through run_process in the workspace root, behind the approval
	// policy. A non-zero exit is a successful call whose content says so; a
	// timeout is reported distinctly; nothing here ever retries.
	[[nodiscard]] auto handle_bash( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// `ask_user`: ambiguity escalation. Headless, it fails with the question in
	// the error; with a terminal it prompts and returns the answer.
	[[nodiscard]] auto handle_ask_user( const tool_args& args, tool_context& context )
		-> result< std::string >;

	// `tool_search`: BM25-class name/description match over the registry.
	// Two-stage disclosure: name + one-line description, full schema on expand.
	// Never mutates the registry.
	[[nodiscard]] auto handle_tool_search( const tool_args& args, tool_context& context,
		const class tool_registry& registry ) -> result< std::string >;

}
