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

	enum class sandbox_network_support {
		unavailable,
		best_effort,
		enforced,
	};

	struct sandbox_profile {
		std::vector< std::filesystem::path > read_paths;
		std::vector< std::filesystem::path > write_paths;

		// a parent grant covers its subtree, so .git must be denied explicitly.
		std::vector< std::filesystem::path > deny_paths;

		bool allow_network = false;
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
			case sandbox_network_support::enforced: return "enforced";
		}

		return "unavailable";
	}

	[[nodiscard]] auto apply_sandbox( const sandbox_profile& profile ) -> status;

	[[nodiscard]] auto terminate_process( std::uint64_t process_id, bool force ) -> status;
	[[nodiscard]] auto terminate_process_tree( std::uint64_t process_id, bool force ) -> status;
	[[nodiscard]] auto process_is_alive( std::uint64_t process_id ) -> bool;

	[[nodiscard]] auto canonicalize( const std::filesystem::path& path )
		-> result< std::filesystem::path>;
	[[nodiscard]] auto temp_directory( ) -> result< std::filesystem::path>;
	[[nodiscard]] auto case_insensitive_paths( ) noexcept -> bool;

	// Windows has MAX_PATH; long paths need the \\?\ prefix. A no-op elsewhere.
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
