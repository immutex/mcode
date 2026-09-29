#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/skills/frontmatter.hxx"

namespace mcode::skills {

	// Where a skill was found. The order is also the collision precedence:
	// project beats user, user beats extension.
	enum class skill_origin { project, user, extension };

	[[nodiscard]] auto to_string( skill_origin origin ) noexcept -> std::string_view;

	// One validated skill. `directory` is the skill's own folder; `body_offset`
	// is where the Markdown body starts inside SKILL.md, so a read can skip the
	// frontmatter without re-parsing.
	struct skill_entry {
		std::string name;
		std::string description;
		std::filesystem::path directory;
		std::filesystem::path file;
		std::size_t body_offset = 0;
		skill_origin origin = skill_origin::project;
		bool disable_model_invocation = false;
	};

	// Discovery state: the entries that validated, and the ones that did not.
	// An invalid skill is reported, never fatal -- the same contract a bad
	// extension manifest has.
	struct discovery_report {
		std::vector< skill_entry > entries;
		std::vector< std::string > rejected;
	};

	struct discovery_options {
		// The repository root. Project skills are searched under it, including
		// nested `.mcode/skills/` directories.
		std::filesystem::path workspace;

		// The user-global root, normally `%APPDATA%\mcode\skills`.
		std::filesystem::path user_root;

		// Extension directories that may carry a `skills/` subtree.
		std::vector< std::filesystem::path > extension_roots;
	};

	// Scans every root, validates each SKILL.md's frontmatter, dedupes by
	// canonical path and resolves name collisions project-first. Never throws
	// on unreadable directories; an unreadable root is skipped.
	[[nodiscard]] auto discover_skills( const discovery_options& options ) -> discovery_report;

	// Reads one discovered skill's body by name. Resolves against the discovered
	// set only, so it cannot become an arbitrary-file read.
	[[nodiscard]] auto read_skill_body( const std::vector< skill_entry >& entries,
		std::string_view name ) -> result< std::string >;

}
