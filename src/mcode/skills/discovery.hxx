#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/skills/frontmatter.hxx"

namespace mcode::skills {

	// Order is the collision precedence: project beats user, user beats extension.
	enum class skill_origin { project, user, extension };

	[[nodiscard]] auto to_string( skill_origin origin ) noexcept -> std::string_view;

	struct skill_entry {
		std::string name;
		std::string description;
		std::filesystem::path directory;
		std::filesystem::path file;
		std::size_t body_offset = 0;
		skill_origin origin = skill_origin::project;
		bool disable_model_invocation = false;
	};

	// An invalid skill is reported, never fatal.
	struct discovery_report {
		std::vector< skill_entry > entries;
		std::vector< std::string > rejected;
	};

	struct discovery_options {
		std::filesystem::path workspace;

		std::filesystem::path user_root;

		std::vector< std::filesystem::path > extension_roots;
	};

	// Never throws on unreadable directories; an unreadable root is skipped.
	[[nodiscard]] auto discover_skills( const discovery_options& options ) -> discovery_report;

	// Resolves against the discovered set only, so it cannot become an arbitrary-file read.
	[[nodiscard]] auto read_skill_body( const std::vector< skill_entry >& entries,
		std::string_view name ) -> result< std::string >;

}
