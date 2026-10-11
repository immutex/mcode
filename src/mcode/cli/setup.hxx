#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mcode::cli {

	// Interactive on a terminal; a flag-driven writer otherwise, which is what CI uses.
	[[nodiscard]] auto run_setup( const std::vector< std::string >& arguments ) -> int;

	[[nodiscard]] auto setup_usage_text( ) -> std::string;

	// Preserves everything outside `[model]` byte for byte: a CRLF file whose
	// section is not recognised gains a second `[model]`, which the loader rejects.
	[[nodiscard]] auto splice_model_section( std::string_view existing,
		std::string_view provider, std::string_view model, std::string_view base_url,
		std::string_view api_key_env ) -> std::string;

}
