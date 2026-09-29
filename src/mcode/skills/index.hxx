#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/skills/discovery.hxx"

namespace mcode::skills {

	// One rendered index line, kept so tests and the CLI can assert on the parts
	// rather than on a concatenated blob.
	struct index_line {
		std::string name;
		std::string description;
		bool hidden = false;
	};

	// True when `disable-model-invocation` keeps the skill out of the prompt.
	// The skill is still discoverable and still readable by name -- only the
	// index omits it.
	[[nodiscard]] auto hidden_from_model( const skill_entry& entry ) noexcept -> bool;

	// One index line per discovered skill, `name — description`, sorted by name,
	// with the description sanitized. `disable-model-invocation` skills are
	// listed with `hidden` set rather than dropped, because the CLI lists every
	// skill while the prompt lists only the visible ones -- one implementation
	// of the rows, two consumers with different filters.
	[[nodiscard]] auto index_lines( const std::vector< skill_entry >& entries )
		-> std::vector< index_line >;

	// Renders the index text for the system prompt. Empty when nothing is
	// visible. Descriptions are sanitized and truncated to the routing length.
	[[nodiscard]] auto render_index( const std::vector< skill_entry >& entries )
		-> std::string;

	// Strips control characters and escapes `<`, `>` and `&`. A description is
	// attacker-controlled text placed into the most privileged position in the
	// request, so it is cleaned before it is rendered anywhere.
	[[nodiscard]] auto sanitize_description( std::string_view description ) -> std::string;

}
