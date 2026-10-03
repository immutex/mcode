#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::instruct {

	struct chain_entry {
		std::filesystem::path file;
		std::string text;
		bool truncated = false;
	};

	// The chain is joined broadest first; a token-fat chain is cut at the broadest entries.
	struct instruction_chain {
		std::string text;
		std::vector< chain_entry > entries;
		std::vector< std::string > warnings;
		std::int64_t estimated_tokens = 0;
	};

	struct chain_options {
		// Ancestors up to the filesystem root contribute too, stopping below a `.git` root.
		std::filesystem::path start;

		std::filesystem::path user_file;

		std::filesystem::path org_file;
	};

	// Closest last, so closest wins on contradiction; one file per directory.
	[[nodiscard]] auto assemble_chain( const chain_options& options ) -> instruction_chain;

}
