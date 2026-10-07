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

	// The exit code a finished run contributes, from its terminal state and the two
	// run-level flags. Shared by exec and the interactive session so the two cannot drift.
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

		// --ask restores prompting for every mutating action and wins over --yolo.
		bool ask = false;

		// --plan is read-only: the engine refuses edits and commands.
		bool plan = false;

		// --continue reopens the newest session for the workspace; --resume names one.
		// An explicit id wins when both are given.
		bool continue_session = false;
		std::string resume_session;

		// --sessions prints the workspace's sessions and exits without running a turn.
		bool list_sessions = false;

		// --worktree runs the session in a fresh git worktree instead of the checkout, so an
		// unattended run cannot touch the user's working tree and `git checkout` undoes it.
		bool worktree = false;
		std::string worktree_name;

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

	// One JSONL event log under the state directory: the unit a resume reopens.
	struct session_ref {
		std::string id;
		std::filesystem::path path;

		// Epoch milliseconds decoded from the id, 0 when the name is not one this
		// program wrote. `--sessions` prints it and `--continue` orders by it.
		std::int64_t started_ms = 0;
		std::uintmax_t size_bytes = 0;
	};

	// `app_data_path( state )/sessions`. Resolving it never creates it.
	[[nodiscard]] auto session_directory( ) -> result< std::filesystem::path >;

	// The id is the canonical workspace root's digest plus the start time, so two
	// workspaces cannot collide on one file and the name is filesystem-safe.
	[[nodiscard]] auto session_id_for( const std::filesystem::path& workspace_root,
		const std::int64_t started_ms ) -> std::string;

	// The workspace's sessions, newest first. An absent sessions directory is an
	// empty list, not an error; an unresolvable state directory is an error.
	[[nodiscard]] auto list_sessions( const std::filesystem::path& workspace_root )
		-> result< std::vector< session_ref > >;

	// The session an explicit id names, or the workspace's newest when `id` is empty.
	// An unknown id is a named `errc::config` error -- it maps to the usage exit --
	// and never falls back to a fresh session.
	[[nodiscard]] auto resolve_session( const std::filesystem::path& workspace_root,
		const std::string_view id ) -> result< session_ref >;

	// The log file this run writes: the resumed session when one was named, else a
	// new file under `sessions/`, which this creates. `event_log::open` replays an
	// existing file, so a resumed session keeps its sequence numbers.
	[[nodiscard]] auto open_session_for_run( const exec_options& options,
		const std::filesystem::path& workspace_root ) -> result< session_ref >;

	// Appends the run's opening event and returns the one-line report the callers
	// print. A resume restores the event log and nothing else: the log records tool
	// calls, tool results and run summaries, so the model's own user and assistant
	// messages are not in it and cannot be replayed into the next turn.
	auto start_session_log( event_log& log, const session_ref& session, bool resumed )
		-> std::string;

	// One line per known session (id, start time, size) on stdout, ending in a
	// `resume with` hint. Exits 0 even when the workspace has no sessions yet; a
	// usage error when the state directory cannot be resolved.
	[[nodiscard]] auto print_sessions( const std::filesystem::path& workspace_root )
		-> exit_code;

	// Creates a detached git worktree under `<workspace>/.mcode/worktrees/` and returns its
	// path. Fails when git is missing or the directory is not a repository, with git's own
	// message rather than a generic one.
	[[nodiscard]] auto create_worktree( const std::filesystem::path& workspace_root,
		std::string_view name ) -> result< std::filesystem::path >;

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
