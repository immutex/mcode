#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcode/instruct/chain.hxx"
#include "mcode/skills/discovery.hxx"

namespace mcode::skills {

	// `skills` must outlive the session: the extension surface borrows it.
	struct session_context {
		discovery_report skills;
		mcode::instruct::instruction_chain chain;
		std::string skill_index;
		std::vector< std::string > warnings;
	};

	struct session_context_options {
		std::filesystem::path workspace;

		std::filesystem::path user_agents_file;
		std::filesystem::path org_agents_file;
		std::filesystem::path user_skills_root;
		std::vector< std::filesystem::path > extension_roots;
	};

	[[nodiscard]] auto assemble_session_context( const session_context_options& options )
		-> session_context;

}
