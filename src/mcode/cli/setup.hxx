#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mcode::cli {

	// `mcode setup`. Interactive when stdin and stdout are a terminal, and a
	// flag-driven writer otherwise, so the same code path serves CI.
	[[nodiscard]] auto run_setup( const std::vector< std::string >& arguments ) -> int;

	// The setup surface, for `mcode --help`.
	[[nodiscard]] auto setup_usage_text( ) -> std::string;

	// Replaces the `[model]` section of a config document, or appends one, and
	// returns the whole document. Everything outside that section is preserved
	// byte for byte, including the line endings the file already uses and the
	// `[models."..."]` pricing blocks.
	//
	// Public and pure because the two ways this can go wrong are silent: a CRLF
	// file whose section is not recognised gains a SECOND `[model]`, which the
	// loader rejects as a duplicate on the user's next start.
	[[nodiscard]] auto splice_model_section( std::string_view existing,
		std::string_view provider, std::string_view model, std::string_view base_url,
		std::string_view api_key_env ) -> std::string;

}
