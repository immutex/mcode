#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::skills {

	// The frontmatter fields the harness reads. Other scalar keys are accepted
	// and unused: the Agent Skills spec reserves them for tooling this slice
	// does not implement.
	struct frontmatter {
		std::string name;
		std::string description;
		bool disable_model_invocation = false;
	};

	// Byte offset just past the closing delimiter of the `---` frontmatter
	// block. An error when the block is absent or never closed, so a caller can
	// strip the block instead of guessing where the body starts.
	[[nodiscard]] auto frontmatter_span( std::string_view text ) -> result< std::size_t >;

	// Parses the `---`-delimited frontmatter block from the head of a SKILL.md.
	//
	// This is not YAML. Scalars and booleans only: a nested table, a list, an
	// anchor, an alias or any other YAML construct is refused with an error
	// rather than half-parsed, because a silently mis-read description is text
	// the harness itself places into the system prompt.
	[[nodiscard]] auto parse_frontmatter( std::string_view text ) -> result< frontmatter >;

}
