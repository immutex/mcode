#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode {

	// past this the oldest run's blobs are dropped, bounding the store.
	inline constexpr std::size_t MAX_RETAINED_RUNS = 20;

	inline constexpr std::string_view SNAPSHOT_INDEX_NAME = "index.jsonl";
	inline constexpr std::string_view SNAPSHOT_BLOB_DIRECTORY = "blobs";

	// Content-addressed store of pre-edit file states, for undo without a git repository.
	class snapshot_store {
	public:
		struct index_entry {
			std::string run;
			std::uint64_t sequence = 0;

			// workspace-relative, forward slashes, so the index is portable across platforms.
			std::string path;

			// false records that the file was absent, which makes a restore delete it.
			bool existed = false;

			// empty exactly when the file did not exist.
			std::string blob;
		};

		explicit snapshot_store( std::filesystem::path root ) : root_( std::move( root ) ) { }

		[[nodiscard]] auto root( ) const noexcept -> const std::filesystem::path& {
			return root_;
		}

		// Records the file's current bytes, or that it did not exist.
		auto capture( const std::filesystem::path& workspace_root,
			const std::filesystem::path& file, const std::string_view run_id ) -> status;

		// Restores every path captured for `run_id`, newest capture per path winning.
		[[nodiscard]] auto restore_last( const std::filesystem::path& workspace_root,
			const std::string_view run_id ) -> result< std::size_t >;

		[[nodiscard]] auto restore_all( const std::filesystem::path& workspace_root )
			-> result< std::size_t >;

	private:
		// A missing index is an empty store; a malformed or unreadable one is an error.
		[[nodiscard]] auto load_index( ) const -> result< std::vector< index_entry > >;

		[[nodiscard]] auto index_path( ) const -> std::filesystem::path;
		[[nodiscard]] auto blob_path( const std::string_view blob ) const
			-> std::filesystem::path;

		auto write_blob( const std::string_view blob, const std::string_view bytes ) -> status;
		[[nodiscard]] auto read_blob( const std::string_view blob, std::string& out ) const
			-> status;

		auto append_entry( const index_entry& entry ) -> status;
		auto rewrite_index( const std::vector< index_entry >& entries ) -> status;

		[[nodiscard]] auto restore_selected( const std::filesystem::path& workspace_root,
			const std::vector< index_entry >& entries ) -> result< std::size_t >;
		[[nodiscard]] auto restore_entry( const std::filesystem::path& workspace_root,
			const index_entry& entry ) -> result< bool >;

		auto evict_over_cap( const std::vector< index_entry >& entries ) -> status;

		std::filesystem::path root_;
	};

	// or an error when `file` is outside `workspace_root`; a path not yet created resolves.
	[[nodiscard]] auto workspace_relative_path( const std::filesystem::path& workspace_root,
		const std::filesystem::path& file ) -> result< std::string >;

}
