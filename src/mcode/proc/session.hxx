#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode::proc {

	inline constexpr std::size_t SESSION_CHUNK_BYTES = 8192;

	// The stderr ring keeps the most recent output only: a status view wants the
	// last words before a crash, not the whole history of a chatty server.
	inline constexpr std::size_t SESSION_STDERR_RING_BYTES = 16u * 1024u;

	// Graceful-shutdown sequencing: close stdin, wait, terminate, wait, and the
	// child is gone. An MCP server exits when its input ends, so the wait is
	// where the graceful path normally ends.
	inline constexpr std::chrono::milliseconds SESSION_GRACE_WAIT{ 5'000 };
	inline constexpr std::chrono::milliseconds SESSION_KILL_WAIT{ 2'000 };

	// Poll step while waiting for an exit; a wait is a bounded loop, not a
	// blocking OS call, so the deadline is enforced in-process.
	inline constexpr std::chrono::milliseconds SESSION_EXIT_POLL{ 10 };

	struct session_options {
		std::string executable;
		std::vector< std::string > args;
		std::string working_directory;

		// Same contract as process_options::sandbox: null spawns unsandboxed.
		const mcode::platform::sandbox_profile* sandbox = nullptr;
	};

	enum class read_kind { data, eof, timeout };

	struct read_result {
		read_kind kind = read_kind::eof;

		// Filled for `data`; empty otherwise.
		std::string data;

		// The OS-level reason the stream ended, when it ended in an error rather
		// than a clean EOF. An unexpected read failure and a clean EOF both mark
		// the child gone, but the diagnostic is worth keeping.
		std::string detail;
	};

	// A long-lived child process whose stdin stays open across writes.
	//
	// This is the primitive `run_process` cannot be: that one closes the child's
	// stdin immediately and drains both pipes to EOF, which is right for a
	// one-shot command and fatal for a protocol session. The session owns the
	// child and its pipes for its whole lifetime; the destructor runs the
	// graceful-shutdown sequence, so a session that goes out of scope cannot
	// leave an orphan behind.
	class session {
	public:
		session( ) = default;
		~session( );

		session( session&& other ) noexcept;
		auto operator=( session&& other ) noexcept -> session&;

		session( const session& ) = delete;
		auto operator=( const session& ) -> session& = delete;

		// Spawns without a shell: the executable and its arguments go to the OS
		// process-creation call verbatim, so a config typo stays an error instead
		// of becoming an injection. The parent environment is inherited, because
		// real servers need PATH and HOME to function.
		[[nodiscard]] static auto spawn( const session_options& options ) -> result< session >;

		// Writes bytes to the child's stdin. The pipe stays open across calls;
		// only `close_stdin` or destruction ends the child's input.
		auto write( std::string_view bytes ) -> status;

		// Signals end-of-input. The child keeps running; it just sees EOF.
		auto close_stdin( ) -> status;

		// Waits up to `timeout` for at least one byte of stdout.
		//
		// `eof` is the event a supervisor reacts to: the child exited or the pipe
		// broke, and either way the session is over. `timeout` is not an error --
		// it is one of three ordinary outcomes, and the caller decides what a
		// silent child means.
		auto read_some( std::chrono::milliseconds timeout ) -> result< read_result >;

		// The child's exit code when it exited within the window; nullopt when it
		// is still running when the window closes.
		auto wait_exit( std::chrono::milliseconds timeout ) -> std::optional< int >;

		// Unconditional termination. The last step of the shutdown sequence.
		auto terminate( ) -> void;

		[[nodiscard]] auto running( ) -> bool;

		// The process id, for diagnostics. Zero for a moved-from or never-spawned
		// session.
		[[nodiscard]] auto id( ) const noexcept -> std::uint64_t;

		// The bounded stderr ring: the most recent output, oldest dropped. Stderr
		// is logging, never a failure signal, so it is buffered on a dedicated
		// drainer thread and can never block the child or the read loop.
		[[nodiscard]] auto stderr_text( ) const -> std::string;

		// Joins the stderr drainer. The child is gone when this is needed, so the
		// drainer is already unwinding; calling it makes the ring's final content
		// deterministic for whoever reads it next.
		auto finalize( ) -> void;

	private:
		struct state;

		std::unique_ptr< state > state_;
	};

}
