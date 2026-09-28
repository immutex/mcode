#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace mcode::tools {

	// The files this session has read, by resolved path and the hash of the
	// content that was read.
	//
	// This is what the read-before-write invariant is enforced against, and it is
	// why the file primitives stay core rather than becoming extensions: a Lua
	// `write` would have to call the host's read-recording API for the check to
	// mean anything, which makes the invariant depend on cooperation. Kept in
	// C++, the check has no cooperation requirement.
	class session_reads {
	public:
		// Records that `path` was read with this content hash. Reading the same
		// file twice updates the hash, which is correct: the later read is the one
		// the model's edits were based on.
		auto record( const std::filesystem::path& path, std::string content_hash ) -> void;

		// The hash recorded for `path`, or nothing when it was never read.
		[[nodiscard]] auto find( const std::filesystem::path& path ) const
			-> std::optional< std::string >;

		[[nodiscard]] auto contains( const std::filesystem::path& path ) const -> bool;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return hashes_.size( ); }

		auto clear( ) noexcept -> void { hashes_.clear( ); }

	private:
		std::map< std::filesystem::path, std::string, std::less<> > hashes_;
	};

}
