#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::skills {

	// Other scalar keys are accepted and unused: the spec reserves them for tooling.
	struct frontmatter {
		std::string name;
		std::string description;
		bool disable_model_invocation = false;

		// Where the body starts: just past the closing delimiter. Carried here so a
		// caller does not recompute it -- and cannot dereference a failed result
		// doing so, which is what the discovery scan used to do.
		std::size_t body_offset = 0;
	};

	// Just past the closing delimiter; an error when the block is absent or never closed.
	[[nodiscard]] auto frontmatter_span( std::string_view text ) -> result< std::size_t >;

	// Not YAML: a nested table, list, anchor or alias is refused rather than half-parsed.
	[[nodiscard]] auto parse_frontmatter( std::string_view text ) -> result< frontmatter >;

}
