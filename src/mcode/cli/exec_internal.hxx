#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/cli/exec.hxx"
#include "mcode/core/error.hxx"

// Shared by the `exec` surface's translation units. Not part of the CLI's
// interface: everything here is an implementation detail of the session store,
// the session log and the JSON stream, and it lives in one place so a change to
// the session id's shape cannot leave one reader disagreeing with another.
namespace mcode::cli::detail {

	// The state directory's subdirectory, and the suffix every session file carries.
	inline constexpr std::string_view SESSION_DIRECTORY_NAME = "sessions";
	inline constexpr std::string_view SESSION_FILE_SUFFIX = ".jsonl";

	// `git worktree add` copies the tree, so it is slower than a status call but bounded.
	inline constexpr std::int64_t WORKTREE_COMMAND_TIMEOUT_MS = 120'000;

	// The label used when the caller names no worktree.
	inline constexpr std::string_view DEFAULT_WORKTREE_NAME = "run";

	// workspace digest, '-', epoch milliseconds padded to 13 digits.
	inline constexpr std::size_t SESSION_DIGEST_LENGTH = 16;
	inline constexpr std::size_t SESSION_STAMP_LENGTH = 13;
	inline constexpr std::size_t SESSION_ID_LENGTH =
		SESSION_DIGEST_LENGTH + 1 + SESSION_STAMP_LENGTH;

	inline constexpr std::string_view SESSION_START_EVENT = "session.start";
	inline constexpr std::string_view SESSION_RESUME_EVENT = "session.resume";

	auto write_line( std::string_view text ) -> void;

	// u8string, not string: a non-ASCII workspace path must not be narrowed to the
	// system code page before it is hashed.
	[[nodiscard]] auto to_utf8( const std::filesystem::path& path ) -> std::string;

	// FNV-1a over the canonical root's UTF-8 bytes, so two workspaces cannot collide
	// on one session id.
	[[nodiscard]] auto workspace_digest( const std::filesystem::path& canonical_root )
		-> std::string;

	// The canonical root when it resolves, the normalized absolute path otherwise. Every
	// entry point derives the digest through here, so `list`, `resolve` and the run that
	// writes the file can never disagree about which workspace a session belongs to.
	[[nodiscard]] auto root_for_digest( const std::filesystem::path& workspace_root )
		-> std::filesystem::path;

	[[nodiscard]] auto session_file_name( std::string_view id ) -> std::string;

	// Zero-padded, so a lexical sort of the names is a chronological sort.
	[[nodiscard]] auto padded_stamp( std::int64_t start_ms ) -> std::string;

	// An id is a hex digest, a dash, and a decimal stamp -- nothing else. A user-supplied
	// id is refused before it is joined to a path, so `../` cannot leave the directory.
	[[nodiscard]] auto is_session_id( std::string_view id ) -> bool;

	// The epoch-milliseconds stamp the id ends with, or 0 when it is not one.
	[[nodiscard]] auto stamp_of_session_id( std::string_view id ) -> std::int64_t;

	// Every well-formed session file in `directory`, newest first. The workspace digest
	// filter belongs to the caller: resolving an id has to see a session from another
	// workspace in order to name it, rather than report that no such session exists.
	[[nodiscard]] auto scan_sessions( const std::filesystem::path& directory )
		-> result< std::vector< session_ref > >;

	[[nodiscard]] auto format_utc( std::int64_t milliseconds ) -> std::string;

}
