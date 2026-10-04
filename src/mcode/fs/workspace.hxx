#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode {

	inline constexpr std::size_t DEFAULT_READ_LINES = 100;
	inline constexpr std::uintmax_t MAX_TEXT_FILE_BYTES = 8u * 1024u * 1024u;
	inline constexpr std::size_t DEFAULT_GLOB_LIMIT = 1000;

	// Above this the content had to come from memory already, so it is a mistake, not an edit.
	inline constexpr std::uintmax_t MAX_WRITE_FILE_BYTES = 10u * 1024u * 1024u;

	struct read_result {
		std::string text;
		std::size_t first_line = 0;
		std::size_t last_line = 0;
		std::size_t total_lines = 0;
		bool truncated = false;
	};

	// The read-before-write invariant: replacing content needs a prior read, creating does not.
	enum class write_mode { create, overwrite };

	struct write_receipt {
		// The hash AFTER the write, so the caller's next edit does not read as stale.
		std::string content_hash;
		std::uintmax_t bytes_written = 0;
	};

	class workspace {
	public:
		[[nodiscard]] static auto open( const std::filesystem::path& root ) -> result< workspace >;

		[[nodiscard]] auto root( ) const noexcept
			-> const std::filesystem::path& { return canonical_root_; }

		[[nodiscard]] auto resolve( std::string_view path ) const
			-> result< std::filesystem::path >;
		[[nodiscard]] auto contains( const std::filesystem::path& absolute ) const -> bool;

		[[nodiscard]] auto glob( std::string_view pattern,
			std::size_t max_results = DEFAULT_GLOB_LIMIT ) const
			-> result< std::vector< std::filesystem::path > >;

		[[nodiscard]] auto read_viewport( std::string_view relative_path, std::size_t offset = 1,
			std::size_t limit = DEFAULT_READ_LINES ) const -> result< read_result >;

		[[nodiscard]] auto read_file( std::string_view relative_path ) const
			-> result< std::string >;
		[[nodiscard]] auto content_hash( std::string_view relative_path ) const
			-> result< std::string >;

		// A sibling temp file renamed over the target, so a crash leaves the original intact.
		[[nodiscard]] auto write_file( std::string_view relative_path, std::string_view content,
			write_mode mode ) -> result< write_receipt >;

		// A query, not an enforcement: model-facing tools must refuse these, the harness must not.
		[[nodiscard]] auto is_protected( const std::filesystem::path& absolute ) const -> bool;

		[[nodiscard]] auto display_path( const std::filesystem::path& path ) const -> std::string;

	private:
		workspace( ) = default;

		std::filesystem::path canonical_root_;
	};

	[[nodiscard]] auto looks_binary( std::string_view bytes ) noexcept -> bool;
	[[nodiscard]] auto hash_bytes( std::string_view bytes ) noexcept -> std::string;

}
