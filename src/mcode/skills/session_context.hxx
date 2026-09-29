#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcode/instruct/chain.hxx"
#include "mcode/skills/discovery.hxx"

namespace mcode::skills {

	// Everything the prompt's sections 10 and 11 need, assembled once at session
	// start. `skills` must outlive the session: the extension surface borrows it.
	struct session_context {
		discovery_report skills;
		mcode::instruct::instruction_chain chain;
		std::string skill_index;
		std::vector< std::string > warnings;
	};

	struct session_context_options {
		std::filesystem::path workspace;

		// Absent paths are skipped, not errors: a machine without a user-global
		// AGENTS.md or an install-directory org file is the normal case.
		std::filesystem::path user_agents_file;
		std::filesystem::path org_agents_file;
		std::filesystem::path user_skills_root;
		std::vector< std::filesystem::path > extension_roots;
	};

	// The one-call seam integration uses: discovery, chain assembly and index
	// rendering in a single function. Nothing here is recomputed per turn.
	[[nodiscard]] auto assemble_session_context( const session_context_options& options )
		-> session_context;

}
