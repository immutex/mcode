#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::instruct {

	// One file in the instruction chain, broadest first.
	struct chain_entry {
		std::filesystem::path file;
		std::string text;
		bool truncated = false;
	};

	// The assembled chain and what happened to it. `warnings` carries the lint
	// findings a caller reports: a token-fat chain is a warning, never a
	// truncation.
	struct instruction_chain {
		std::string text;
		std::vector< chain_entry > entries;
		std::vector< std::string > warnings;
		std::int64_t estimated_tokens = 0;
	};

	struct chain_options {
		// Walk starts here; ancestors up to the filesystem root contribute too,
		// stopping below a detected repository root (`.git`).
		std::filesystem::path start;

		// The user-global file, normally `%APPDATA%\mcode\AGENTS.md`. Absent when
		// the platform seam cannot resolve one.
		std::filesystem::path user_file;

		// The org-managed file beside the binary, `<install>/AGENTS.md`. Absent
		// when the platform seam cannot resolve one.
		std::filesystem::path org_file;
	};

	// Discovers and assembles the instruction chain: org → user → root → … →
	// cwd, closest last so closest wins on contradiction. One file per
	// directory, first match of AGENTS.md, CLAUDE.md, GEMINI.md.
	//
	// Byte caps are safety limits: a file over the per-file cap is truncated,
	// and when the chain over the chain cap the BROADDEST file is cut first,
	// each cut leaving a one-line pointer the model can `read`. The token
	// budget is reported as a warning and never truncates.
	[[nodiscard]] auto assemble_chain( const chain_options& options ) -> instruction_chain;

}
