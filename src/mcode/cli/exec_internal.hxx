#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/cli/exec.hxx"
#include "mcode/core/error.hxx"
#include "mcode/agent/loop.hxx"
#include "mcode/model/client.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli::detail {

	inline constexpr std::string_view SESSION_DIRECTORY_NAME = "sessions";
	inline constexpr std::string_view SESSION_FILE_SUFFIX = ".jsonl";

	// `git worktree add` copies the tree, so it is slower than a status call but bounded.
	inline constexpr std::int64_t WORKTREE_COMMAND_TIMEOUT_MS = 120'000;

	inline constexpr std::string_view DEFAULT_WORKTREE_NAME = "run";

	// workspace digest, '-', epoch milliseconds padded to 13 digits.
	inline constexpr std::size_t SESSION_DIGEST_LENGTH = 16;
	inline constexpr std::size_t SESSION_STAMP_LENGTH = 13;
	inline constexpr std::size_t SESSION_ID_LENGTH =
		SESSION_DIGEST_LENGTH + 1 + SESSION_STAMP_LENGTH;

	inline constexpr std::string_view SESSION_START_EVENT = "session.start";
	inline constexpr std::string_view SESSION_RESUME_EVENT = "session.resume";

	auto write_line( std::string_view text ) -> void;

	// u8string, not string: a non-ASCII workspace path must not be narrowed to the system
	// code page before it is hashed.
	[[nodiscard]] auto to_utf8( const std::filesystem::path& path ) -> std::string;

	// FNV-1a over the canonical root's UTF-8 bytes, so two workspaces cannot collide on one id.
	[[nodiscard]] auto workspace_digest( const std::filesystem::path& canonical_root )
		-> std::string;

	// Canonical when it resolves, the normalized absolute path otherwise. Every entry point
	// derives the digest through it, so list, resolve and the writer cannot disagree.
	[[nodiscard]] auto root_for_digest( const std::filesystem::path& workspace_root )
		-> std::filesystem::path;

	[[nodiscard]] auto session_file_name( std::string_view id ) -> std::string;

	[[nodiscard]] auto padded_stamp( std::int64_t start_ms ) -> std::string;

	// A user-supplied id is refused before it is joined to a path, so `../` cannot escape.
	[[nodiscard]] auto is_session_id( std::string_view id ) -> bool;

	[[nodiscard]] auto stamp_of_session_id( std::string_view id ) -> std::int64_t;

	// The digest filter belongs to the caller: resolving an id must see foreign sessions.
	[[nodiscard]] auto scan_sessions( const std::filesystem::path& directory )
		-> result< std::vector< session_ref > >;

	[[nodiscard]] auto format_utc( std::int64_t milliseconds ) -> std::string;

	inline constexpr std::string_view SNAPSHOT_DIRECTORY = "snapshots";

	// One function because the precedence is a security decision: `--ask` follows `--yolo`.
	[[nodiscard]] auto resolve_approval_policy( const exec_options& options,
		const mcode::config::merged_config& config, bool interactive )
		-> mcode::perm::permission_engine::options;

	// Mirrors text deltas to the stream when the run is not in `--json` mode, where the
	// bus subscription already emits them.
	class streaming_client final : public mcode::model::model_client {
	public:
		streaming_client( mcode::model::model_client& inner, std::FILE* out, bool json )
			: inner_( inner ), out_( out ), json_( json ) { }

		auto stream( const mcode::model::stream_request& request,
			const mcode::model::event_sink& sink ) -> mcode::status override;

	private:
		mcode::model::model_client& inner_;
		std::FILE* out_;
		bool json_;
	};

	// The same failure reaches the human on stderr and a `--json` consumer on the
	// stream, and both get the exit code the error maps to.
	[[nodiscard]] auto report_failure( json_stream& stream, const mcode::error& failure ) -> int;

	// The run's closing report: the one-line summary, the verification verdict, and
	// the exit code, on both channels. Returns that code for the caller to pass on.
	[[nodiscard]] auto report_run_outcome( json_stream& stream, const agent_loop& loop,
		const turn_outcome& outcome, exit_code code, bool json, bool verbose ) -> int;

}
