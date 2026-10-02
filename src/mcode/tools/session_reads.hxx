#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace mcode::tools {

	// resolved path -> hash of the content read; the read-before-write invariant checks this
	class session_reads {
	public:
		// a second read overwrites: the later read is what the edits were based on
		auto record( const std::filesystem::path& path, std::string content_hash ) -> void;

		[[nodiscard]] auto find( const std::filesystem::path& path ) const
			-> std::optional< std::string >;

		[[nodiscard]] auto contains( const std::filesystem::path& path ) const -> bool;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return hashes_.size( ); }

		auto clear( ) noexcept -> void { hashes_.clear( ); }

	private:
		std::map< std::filesystem::path, std::string, std::less<> > hashes_;
	};

}
