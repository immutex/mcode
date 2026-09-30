#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

// The seven OS seams. Everything platform-specific lives behind one of
// these, and no `#ifdef` appears in portable code -- that rule is the whole point
// of the file, because retrofitting a platform is how a core ends up
// Windows-shaped.
//
// M0 ships interfaces and honest stubs. A stub that reports `unsupported` is
// correct; a stub that pretends to work is not, so every fallible call returns
// `result` and no operation silently no-ops.

namespace mcode::platform {

	// --- 1. PtySession ------------------------------------------------------
	// The largest divergence in the codebase: ConPTY with blocking I/O on
	// dedicated threads versus openpty/posix_spawn.
	class pty_session {
	public:
		[[nodiscard]] static auto supported( ) noexcept -> bool;

		// Spawns a child attached to a pseudo-terminal. M0 does not implement it;
		// the interface exists so the process layer has somewhere to plug in.
		[[nodiscard]] static auto spawn( const std::filesystem::path& executable,
			const std::vector< std::string >& arguments ) -> result< pty_session >;

		[[nodiscard]] auto write( std::string_view data ) -> status;
		[[nodiscard]] auto read( ) -> result< std::string >;

		// Reported in cells, not pixels. The source of truth differs per OS
		// (seam 6), so this is the portable view.
		auto resize( int columns, int rows ) -> status;

		[[nodiscard]] auto exit_code( ) const noexcept -> std::optional< int >;

	private:
		pty_session( ) = default;
	};

	// --- 2. Sandbox ---------------------------------------------------------
	// Three incompatible models: Windows token + Job Object is code, Seatbelt is
	// a profile, Landlock is syscalls with runtime ABI probing. Filesystem
	// confinement and network denial are two enums, not one: on Windows the
	// restricted-token tier needs no administrator while a WFP egress deny does,
	// so a single value claiming "supported" while the network is open would be
	// exactly the false claim this seam exists to prevent.
	enum class sandbox_capability {
		// Nothing is enforced on this platform or runtime. Callers must fail
		// closed rather than assume isolation.
		unavailable,
		// Filesystem confinement (read_paths / write_paths) is enforced.
		filesystem,
		// Filesystem confinement and egress denial are both enforced.
		full,
	};

	enum class sandbox_network_support {
		// No egress denial exists.
		unavailable,
		// Egress denial is attempted but cannot be guaranteed on this runtime.
		best_effort,
		// Egress is denied at the OS layer when allow_network is false.
		enforced,
	};

	struct sandbox_profile {
		std::vector< std::filesystem::path > read_paths;
		std::vector< std::filesystem::path > write_paths;
		bool allow_network = false;
	};

	[[nodiscard]] auto sandbox_capability_level( ) noexcept -> sandbox_capability;
	[[nodiscard]] auto sandbox_network_level( ) noexcept -> sandbox_network_support;
	[[nodiscard]] auto sandbox_mechanism( ) noexcept -> std::string_view;

	// Applies the profile to the current process, or to a child at spawn time.
	[[nodiscard]] auto apply_sandbox( const sandbox_profile& profile ) -> status;

	// --- 3. Termination -----------------------------------------------------
	// Windows has no real signals: SetConsoleCtrlHandler plus TerminateProcess
	// plus Job close. Child-tree kill differs per OS, so this is a seam rather
	// than a portable `kill`.
	[[nodiscard]] auto terminate_process( std::uint64_t process_id, bool force ) -> status;
	[[nodiscard]] auto terminate_process_tree( std::uint64_t process_id, bool force ) -> status;
	[[nodiscard]] auto process_is_alive( std::uint64_t process_id ) -> bool;

	// --- 4. fs helpers ------------------------------------------------------
	// Canonicalization with case-fold policy, realpath, temp dir, long-path
	// prefixing. `workspace` depends on these being correct, because a boundary
	// that compares uncanonicalized paths is not a boundary.
	[[nodiscard]] auto canonicalize( const std::filesystem::path& path )
		-> result< std::filesystem::path>;
	[[nodiscard]] auto temp_directory( ) -> result< std::filesystem::path>;
	[[nodiscard]] auto case_insensitive_paths( ) noexcept -> bool;

	// Windows has MAX_PATH; long paths need the \\?\ prefix. A no-op elsewhere.
	[[nodiscard]] auto to_extended_path( const std::filesystem::path& path )
		-> std::filesystem::path;

	// --- 5. paths -----------------------------------------------------------
	enum class data_kind {
		config,
		data,
		cache,
		state,
		log,
	};

	[[nodiscard]] auto app_data_path( data_kind kind ) -> result< std::filesystem::path>;

	// The directory the running binary sits in. The bundled extensions are copied
	// beside it, so this is how the loader finds them.
	//
	// A seam rather than an inline platform check: the previous version called
	// GetModuleFileNameA directly in the CLI, which does not compile on POSIX at
	// all -- `DWORD` and the function are both Win32-only. `24` requires the
	// platform decision to live here so portable code never sees it.
	[[nodiscard]] auto executable_directory( ) -> result< std::filesystem::path >;

	// --- 6. ResizeSource ----------------------------------------------------
	// SIGWINCH versus WINDOW_BUFFER_SIZE_EVENT versus in-band DECSET 2048. The
	// interface is a poll rather than a callback so the loop stays single-threaded.
	[[nodiscard]] auto terminal_size( ) -> result< std::pair< int, int > >;
	[[nodiscard]] auto terminal_size_changed( ) -> bool;

	// --- 7. FileWatch -------------------------------------------------------
	// ReadDirectoryChangesW versus inotify versus FSEvents. Deliberately the odd
	// one out: the real implementation is a Lua extension over a
	// small core API, so M0 ships only the shape.
	enum class file_event_kind {
		created,
		modified,
		removed,
		renamed,
	};

	struct file_event {
		file_event_kind kind = file_event_kind::modified;
		std::filesystem::path path;
	};

	class file_watcher {
	public:
		[[nodiscard]] static auto supported( ) noexcept -> bool;

		[[nodiscard]] static auto create( const std::filesystem::path& directory, bool recursive )
			-> result< file_watcher >;

		// Non-blocking: returns whatever has arrived. A blocking wait would put a
		// platform-specific wait primitive on the loop thread.
		[[nodiscard]] auto poll( ) -> std::vector< file_event >;

	private:
		file_watcher( ) = default;
	};

}
