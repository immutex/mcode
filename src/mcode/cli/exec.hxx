#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/core/error.hxx"
#include "mcode/events/bus.hxx"

namespace mcode::cli {

	// Exit codes, part of the interface: a consumer script branches on these.
	enum class exit_code : int {
		success = 0,
		verification_failed = 1,
		usage_error = 2,
		budget_exhausted = 3,
		provider_error = 4,
		permission_denied = 5,
		interrupted = 130,
	};

	// The mapping is deliberately total: an unmapped error is a bug, not a fallthrough to 1.
	[[nodiscard]] auto exit_code_for( errc code ) -> exit_code;
	[[nodiscard]] auto to_int( exit_code code ) noexcept -> int;

	[[nodiscard]] auto exit_code_for_run( const mcode::turn_outcome& outcome, bool budget_exhausted,
		bool permission_denied ) -> exit_code;

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
		bool ask = false;
		bool plan = false;
		bool continue_session = false;
		std::string resume_session;
		bool list_sessions = false;
		bool worktree = false;
		std::string worktree_name;

		// Empty means "not set", so the merged config's sandbox.approval can win.
		std::string approval;

		std::vector< std::string > add_dirs;

		// Empty is legitimate, reported as `no-tests`, never as verified.
		std::string verify_command;

		// Unknown flags are never ignored silently.
		std::vector< std::string > unknown_arguments;
	};

	[[nodiscard]] auto parse_exec_options( const std::vector< std::string >& arguments )
		-> result< exec_options >;

	[[nodiscard]] auto argument_error_text( const std::vector< std::string >& arguments )
		-> std::optional< std::string >;

	struct session_ref {
		std::string id;
		std::filesystem::path path;

		// 0 when the name is not one this program wrote.
		std::int64_t started_ms = 0;
		std::uintmax_t size_bytes = 0;
	};

	// Resolving it never creates it.
	[[nodiscard]] auto session_directory( ) -> result< std::filesystem::path >;

	[[nodiscard]] auto session_id_for( const std::filesystem::path& workspace_root,
		const std::int64_t started_ms ) -> std::string;

	// An absent sessions directory is an empty list, not an error.
	[[nodiscard]] auto list_sessions( const std::filesystem::path& workspace_root )
		-> result< std::vector< session_ref > >;

	// An unknown id is a named `errc::config` error and never falls back to a fresh session.
	[[nodiscard]] auto resolve_session( const std::filesystem::path& workspace_root,
		const std::string_view id ) -> result< session_ref >;

	// `event_log::open` replays an existing file, so a resumed session keeps its sequence numbers.
	[[nodiscard]] auto open_session_for_run( const exec_options& options,
		const std::filesystem::path& workspace_root ) -> result< session_ref >;

	[[nodiscard]] auto new_session_ref( const std::filesystem::path& workspace_root )
		-> result< session_ref >;

	// Stops at the first user message, so a picker over many sessions stays cheap.
	[[nodiscard]] auto session_opening_line( const std::filesystem::path& path,
		std::size_t max_chars = 72 ) -> std::string;

	// Appends the run's opening event and returns the report line the callers print.
	auto start_session_log( event_log& log, const session_ref& session, bool resumed )
		-> std::string;

	[[nodiscard]] auto restore_transcript( const event_log& log )
		-> std::vector< model::message >;

	[[nodiscard]] auto print_sessions( const std::filesystem::path& workspace_root )
		-> exit_code;

	[[nodiscard]] auto create_worktree( const std::filesystem::path& workspace_root,
		std::string_view name ) -> result< std::filesystem::path >;

	// One JSON object per line on stdout; diagnostics go to stderr. The stream ends with
	// exactly ONE run.end, which is how a consumer tells a finished run from a cut-off one.
	class json_stream {
	public:
		explicit json_stream( bool enabled );

		auto emit_run_start( std::string_view prompt ) -> void;
		auto emit_event( const events::event& value ) -> void;
		auto emit_run_end( exit_code code, std::string_view summary,
			verification_verdict verdict = verification_verdict::no_tests,
			std::string_view verification_detail = { } ) -> void;

		// Called on every emit: a headless consumer reading a pipe must see each line as it happens.
		auto flush( ) -> void;

		[[nodiscard]] auto enabled( ) const noexcept -> bool { return enabled_; }
		[[nodiscard]] auto run_end_emitted( ) const noexcept -> bool { return run_end_emitted_; }
		[[nodiscard]] auto lines_emitted( ) const noexcept -> std::uint64_t { return lines_; }

		// Warns on stderr when the run ended without a run.end.
		~json_stream( );

	private:
		bool enabled_ = false;
		bool run_end_emitted_ = false;
		std::uint64_t lines_ = 0;
	};

	[[nodiscard]] auto usage_text( std::string_view program ) -> std::string;

}
