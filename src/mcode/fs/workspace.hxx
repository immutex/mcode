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

	// The largest file a tool may create or replace. Above this the content had to
	// come from somewhere in memory already, and a single write that large is a
	// mistake rather than an edit.
	inline constexpr std::uintmax_t MAX_WRITE_FILE_BYTES = 10u * 1024u * 1024u;

	struct read_result {
		std::string text;
		std::size_t first_line = 0;
		std::size_t last_line = 0;
		std::size_t total_lines = 0;
		bool truncated = false;
	};

	// `create` requires the path to be absent, `overwrite` requires it to exist.
	// The distinction is the whole read-before-write invariant: replacing content
	// needs a prior read, creating a file does not.
	enum class write_mode { create, overwrite };

	struct write_receipt {
		// The hash AFTER the write, which is what the caller records so its own
		// next edit does not read as stale.
		std::string content_hash;
		std::uintmax_t bytes_written = 0;
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

		// Creates or replaces a file, atomically.
		//
		// Writes a sibling temp file and renames it over the target, so a crash
		// mid-write leaves the original intact rather than truncated. The content
		// is bounded by MAX_WRITE_FILE_BYTES.
		//
		// Does NOT refuse `.mcode/` or `.git/`: the harness has to write artifacts
		// under `.mcode/artifacts/`. That denial is actor-scoped and belongs to the
		// tool layer, which consults `is_protected` -- see `12` and the rule in
		// `31`. Moving it here would give the model the same access the harness has.
		[[nodiscard]] auto write_file( std::string_view relative_path, std::string_view content,
			write_mode mode ) -> result< write_receipt >;

		// True for a path under `.mcode/` or `.git/` at the workspace root.
		//
		// A query rather than an enforcement so the caller decides: model-facing
		// tools must refuse these, the harness's own artifact spill must not.
		[[nodiscard]] auto is_protected( const std::filesystem::path& absolute ) const -> bool;

		[[nodiscard]] auto display_path( const std::filesystem::path& path ) const -> std::string;

	private:
		workspace( ) = default;

		std::filesystem::path canonical_root_;
	};

	[[nodiscard]] auto looks_binary( std::string_view bytes ) noexcept -> bool;
	[[nodiscard]] auto hash_bytes( std::string_view bytes ) noexcept -> std::string;

}
