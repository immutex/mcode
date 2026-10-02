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

	// a status view wants the last words before a crash, not the whole history
	inline constexpr std::size_t SESSION_STDERR_RING_BYTES = 16u * 1024u;

	// close stdin, wait, terminate, wait; an MCP server exits when its input ends
	inline constexpr std::chrono::milliseconds SESSION_GRACE_WAIT{ 5'000 };
	inline constexpr std::chrono::milliseconds SESSION_KILL_WAIT{ 2'000 };

	// waits are bounded in-process loops, not blocking OS calls
	inline constexpr std::chrono::milliseconds SESSION_EXIT_POLL{ 10 };

	struct session_options {
		std::string executable;
		std::vector< std::string > args;
		std::string working_directory;

		// same contract as process_options::sandbox: null spawns unsandboxed
		const mcode::platform::sandbox_profile* sandbox = nullptr;
	};

	enum class read_kind { data, eof, timeout };

	struct read_result {
		read_kind kind = read_kind::eof;

		std::string data;

		// the OS-level reason the stream ended; empty on a clean EOF
		std::string detail;
	};

	// a protocol session needs stdin open across writes, which run_process closes immediately
	class session {
	public:
		session( ) = default;
		~session( );

		session( session&& other ) noexcept;
		auto operator=( session&& other ) noexcept -> session&;

		session( const session& ) = delete;
		auto operator=( const session& ) -> session& = delete;

		// no shell: a config typo stays an error instead of becoming an injection
		[[nodiscard]] static auto spawn( const session_options& options ) -> result< session >;

		// the pipe stays open across calls; only close_stdin or destruction ends the input
		auto write( std::string_view bytes ) -> status;

		// the child keeps running; it just sees EOF
		auto close_stdin( ) -> status;

		// timeout is one of three ordinary outcomes, not an error
		auto read_some( std::chrono::milliseconds timeout ) -> result< read_result >;

		// nullopt when the child is still running as the window closes
		auto wait_exit( std::chrono::milliseconds timeout ) -> std::optional< int >;

		auto terminate( ) -> void;

		[[nodiscard]] auto running( ) -> bool;

		// zero for a moved-from or never-spawned session
		[[nodiscard]] auto id( ) const noexcept -> std::uint64_t;

		// bounded ring, oldest dropped; a drainer thread keeps it off the child's path
		[[nodiscard]] auto stderr_text( ) const -> std::string;

		// joins the drainer, making the ring's final content deterministic
		auto finalize( ) -> void;

	private:
		struct state;

		std::unique_ptr< state > state_;
	};

}
