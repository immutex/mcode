#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/events/bus.hxx"

namespace mcode::cli {

	// Exit codes, part of the interface. A consumer script branches on these, so the
	// numbers are an interface and a test asserts each one.
	enum class exit_code : int {
		success = 0,
		verification_failed = 1,
		usage_error = 2,
		budget_exhausted = 3,
		provider_error = 4,
		permission_denied = 5,
		interrupted = 130,
	};

	// Maps an internal error to the documented exit code. The mapping is
	// deliberately total: an unmapped error is a bug, not a fallthrough to 1.
	[[nodiscard]] auto exit_code_for( errc code ) -> exit_code;
	[[nodiscard]] auto to_int( exit_code code ) noexcept -> int;

	struct exec_options {
		std::string prompt;
		std::string working_directory;
		std::string model;

		std::uint32_t max_steps = 0;
		double max_budget_usd = 0.0;

		bool json = false;
		bool verbose = false;
		bool no_extensions = false;
		bool yolo = false;

		// `never` | `on-request` | `always`, from --approval or the merged
		// config's sandbox.approval. Empty means "not set", so the config
		// value can win.
		std::string approval;

		// Extra workspace roots from --add-dir, canonicalized by the caller.
		std::vector< std::string > add_dirs;

		// Remaining argv after flag extraction. Non-empty means the caller passed
		// something we did not recognise, which is a usage error -- unknown flags
		// are never ignored silently (AGENTS.md §Correctness).
		std::vector< std::string > unknown_arguments;
	};

	// Parses argv. Unknown flags land in `unknown_arguments` rather than being
	// dropped, so `main` can refuse them with a usage error and a hint.
	[[nodiscard]] auto parse_exec_options( const std::vector< std::string >& arguments )
		-> result< exec_options >;

	// The `exec --json` stream. One JSON object per line on stdout;
	// diagnostics go to stderr.
	//
	// The stream starts with run.start and ends with exactly ONE run.end carrying
	// the exit code. That pairing is the contract consumers rely on to know a run
	// finished rather than was cut off, so the writer enforces it.
	class json_stream {
	public:
		explicit json_stream( bool enabled );

		auto emit_run_start( std::string_view prompt ) -> void;
		auto emit_event( const events::event& value ) -> void;
		auto emit_run_end( exit_code code, std::string_view summary ) -> void;

		// Flushes stdout. Called on every emit: a headless consumer reading a pipe
		// must see each line as it happens, or a long run looks hung.
		auto flush( ) -> void;

		[[nodiscard]] auto enabled( ) const noexcept -> bool { return enabled_; }
		[[nodiscard]] auto run_end_emitted( ) const noexcept -> bool { return run_end_emitted_; }
		[[nodiscard]] auto lines_emitted( ) const noexcept -> std::uint64_t { return lines_; }

		// A stream whose destructor runs without a run.end is a truncated stream;
		// the count makes that visible in a test rather than silent in production.
		~json_stream( );

	private:
		bool enabled_ = false;
		bool run_end_emitted_ = false;
		std::uint64_t lines_ = 0;
	};

	// Human-readable usage text for `--help` and for a usage error.
	[[nodiscard]] auto usage_text( std::string_view program ) -> std::string;

}
