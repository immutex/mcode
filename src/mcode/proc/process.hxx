#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode {

	inline constexpr std::size_t DEFAULT_MAX_OUTPUT_BYTES = 1u * 1024u * 1024u;
	inline constexpr std::size_t PIPE_CHUNK_BYTES = 8192;

	struct process_options {
		std::string executable;
		std::vector< std::string > args;
		std::string working_directory;
		std::map< std::string, std::string, std::less<> > environment;
		bool scrub_environment = true;
		std::chrono::milliseconds timeout{ 60'000 };
		std::size_t max_output_bytes = DEFAULT_MAX_OUTPUT_BYTES;

		// null spawns unsandboxed
		const mcode::platform::sandbox_profile* sandbox = nullptr;
	};

	struct process_result {
		int exit_code = -1;
		std::string stdout_text;
		std::string stderr_text;
		bool timed_out = false;
		bool output_truncated = false;
		std::chrono::milliseconds elapsed{ 0 };
	};

	[[nodiscard]] auto run_process( const process_options& options ) -> result< process_result >;
	[[nodiscard]] auto find_executable( std::string_view name ) -> result< std::string >;

	[[nodiscard]] auto minimal_environment( ) -> std::map< std::string, std::string, std::less<> >;

}
