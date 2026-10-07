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

	// Retained capture groups. Past this the oldest run's blobs are dropped, so a long
	// session cannot grow the store without bound.
	inline constexpr std::size_t MAX_RETAINED_RUNS = 20;

	inline constexpr std::string_view SNAPSHOT_INDEX_NAME = "index.jsonl";
	inline constexpr std::string_view SNAPSHOT_BLOB_DIRECTORY = "blobs";

	// Content-addressed store of pre-edit file states, so an edit can be undone in a workspace
	// that is not a git repository. Blobs live under `<root>/blobs/<hash>`, one append-only
	// index entry per capture under `<root>/index.jsonl`.
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

		// Records the file's current bytes, or that it did not exist. The store directory is
		// created on the first write, so constructing one costs nothing.
		auto capture( const std::filesystem::path& workspace_root,
			const std::filesystem::path& file, const std::string_view run_id ) -> status;

		// Restores every path captured for `run_id`, newest capture per path winning.
		[[nodiscard]] auto restore_last( const std::filesystem::path& workspace_root,
			const std::string_view run_id ) -> result< std::size_t >;

		// The same across every run the index holds.
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

		// Drops the oldest runs past MAX_RETAINED_RUNS, their blobs included.
		auto evict_over_cap( const std::vector< index_entry >& entries ) -> status;

		std::filesystem::path root_;
	};

	// The workspace-relative, forward-slash path of `file`, or an error when it is outside
	// `workspace_root`. A path that has not been created yet still resolves.
	[[nodiscard]] auto workspace_relative_path( const std::filesystem::path& workspace_root,
		const std::filesystem::path& file ) -> result< std::string >;

}
