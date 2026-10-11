#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::platform {

	class pty_session {
	public:
		[[nodiscard]] static auto supported( ) noexcept -> bool;

		[[nodiscard]] static auto spawn( const std::filesystem::path& executable,
			const std::vector< std::string >& arguments ) -> result< pty_session >;

		[[nodiscard]] auto write( std::string_view data ) -> status;
		[[nodiscard]] auto read( ) -> result< std::string >;

		auto resize( int columns, int rows ) -> status;

		[[nodiscard]] auto exit_code( ) const noexcept -> std::optional< int >;

	private:
		pty_session( ) = default;
	};

	enum class sandbox_capability {
		unavailable,
		// Windows only: integrity levels have no read-down rule, so reads stay open.
		write_boundary,
		filesystem,
		full,
	};

	// The OS rule is the boundary; `HTTP(S)_PROXY` is only how a compliant tool finds it.
	struct egress_endpoint {
		std::string host = "127.0.0.1";
		std::uint16_t http_port = 0;
		std::uint16_t socks_port = 0;

		[[nodiscard]] auto active( ) const noexcept -> bool {
			return http_port != 0 || socks_port != 0;
		}
	};

	enum class sandbox_network_support {
		unavailable,
		// The env var points at a proxy but nothing stops a process that ignores it.
		best_effort,
		// Direct egress is blocked by the OS and only the proxy port is reachable.
		mediated,
		// As `mediated`, and an unprivileged child cannot bypass it.
		enforced,
	};

	struct sandbox_profile {
		std::vector< std::filesystem::path > read_paths;
		std::vector< std::filesystem::path > write_paths;

		// a parent grant covers its subtree, so .git must be denied explicitly.
		std::vector< std::filesystem::path > deny_paths;

		bool allow_network = false;

		// With `allow_network`, the OS rule permits only this endpoint.
		egress_endpoint egress;
	};

	[[nodiscard]] auto sandbox_capability_level( ) noexcept -> sandbox_capability;
	[[nodiscard]] auto sandbox_network_level( ) noexcept -> sandbox_network_support;
	[[nodiscard]] auto sandbox_mechanism( ) noexcept -> std::string_view;

	[[nodiscard]] constexpr auto to_string( const sandbox_capability value ) noexcept
		-> std::string_view {
		switch ( value ) {
			case sandbox_capability::unavailable: return "unavailable";
			case sandbox_capability::write_boundary: return "write-boundary";
			case sandbox_capability::filesystem: return "filesystem";
			case sandbox_capability::full: return "full";
		}

		return "unavailable";
	}

	[[nodiscard]] constexpr auto to_string( const sandbox_network_support value ) noexcept
		-> std::string_view {
		switch ( value ) {
			case sandbox_network_support::unavailable: return "unavailable";
			case sandbox_network_support::best_effort: return "best-effort";
			case sandbox_network_support::mediated: return "mediated";
			case sandbox_network_support::enforced: return "enforced";
		}

		return "unavailable";
	}

	[[nodiscard]] auto current_process_id( ) noexcept -> std::uint64_t;

	[[nodiscard]] auto process_is_alive( std::uint64_t process_id ) -> bool;

	[[nodiscard]] auto canonicalize( const std::filesystem::path& path )
		-> result< std::filesystem::path>;
	[[nodiscard]] auto temp_directory( ) -> result< std::filesystem::path >;

	// A writable temp directory of our own, under the system one: on Windows a write
	// grant is an integrity label that propagates, so granting %TEMP% would relabel
	// the user's whole temp tree on every command.
	inline constexpr std::string_view SANDBOX_TEMP_PREFIX = "mcode-run-";

	[[nodiscard]] auto sandbox_temp_directory( ) -> result< std::filesystem::path >;
	[[nodiscard]] auto case_insensitive_paths( ) noexcept -> bool;

	// Windows needs the \\?\ prefix for long paths; a no-op elsewhere.
	[[nodiscard]] auto to_extended_path( const std::filesystem::path& path )
		-> std::filesystem::path;

	enum class data_kind {
		config,
		data,
		cache,
		state,
		log,
	};

	[[nodiscard]] auto app_data_path( data_kind kind ) -> result< std::filesystem::path>;

	// bundled extensions are copied beside the binary; this is how the loader finds them.
	[[nodiscard]] auto executable_directory( ) -> result< std::filesystem::path >;

	// polled, not a callback: SIGWINCH, WINDOW_BUFFER_SIZE_EVENT, or DECSET 2048.
	[[nodiscard]] auto terminal_size( ) -> result< std::pair< int, int > >;
	[[nodiscard]] auto terminal_size_changed( ) -> bool;

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

		[[nodiscard]] auto poll( ) -> std::vector< file_event >;

	private:
		file_watcher( ) = default;
	};

}
