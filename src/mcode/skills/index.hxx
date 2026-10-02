#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/skills/discovery.hxx"

namespace mcode::skills {

	struct index_line {
		std::string name;
		std::string description;
		bool hidden = false;
	};

	// Keeps the skill out of the prompt only; it stays discoverable and readable by name.
	[[nodiscard]] auto hidden_from_model( const skill_entry& entry ) noexcept -> bool;

	// `disable-model-invocation` skills are listed with `hidden` set rather than dropped.
	[[nodiscard]] auto index_lines( const std::vector< skill_entry >& entries )
		-> std::vector< index_line >;

	[[nodiscard]] auto render_index( const std::vector< skill_entry >& entries )
		-> std::string;

	// The description is attacker-controlled and lands in the most privileged request position.
	[[nodiscard]] auto sanitize_description( std::string_view description ) -> std::string;

}
