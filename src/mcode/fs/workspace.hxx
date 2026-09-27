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

	struct read_result {
		std::string text;
		std::size_t first_line = 0;
		std::size_t last_line = 0;
		std::size_t total_lines = 0;
		bool truncated = false;
	};

	class workspace {
	public:
		[[nodiscard]] static auto open( const std::filesystem::path& root ) -> result< workspace >;

		[[nodiscard]] auto root( ) const noexcept -> const std::filesystem::path& { return canonical_root_; }

		[[nodiscard]] auto resolve( std::string_view path ) const -> result< std::filesystem::path >;
		[[nodiscard]] auto contains( const std::filesystem::path& absolute ) const -> bool;

		[[nodiscard]] auto glob( std::string_view pattern,
			std::size_t max_results = DEFAULT_GLOB_LIMIT ) const -> result< std::vector< std::filesystem::path > >;

		[[nodiscard]] auto read_viewport( std::string_view relative_path, std::size_t offset = 1,
			std::size_t limit = DEFAULT_READ_LINES ) const -> result< read_result >;

		[[nodiscard]] auto read_file( std::string_view relative_path ) const -> result< std::string >;
		[[nodiscard]] auto content_hash( std::string_view relative_path ) const -> result< std::string >;

		[[nodiscard]] auto display_path( const std::filesystem::path& path ) const -> std::string;

	private:
		workspace( ) = default;

		std::filesystem::path canonical_root_;
	};

	[[nodiscard]] auto looks_binary( std::string_view bytes ) noexcept -> bool;
	[[nodiscard]] auto hash_bytes( std::string_view bytes ) noexcept -> std::string;

}
