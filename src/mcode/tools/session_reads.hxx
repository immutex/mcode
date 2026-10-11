#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace mcode::tools {

	// resolved path -> hash of the content read; the read-before-write invariant checks this
	class session_reads {
	public:
		auto record( const std::filesystem::path& path, std::string content_hash ) -> void;

		[[nodiscard]] auto find( const std::filesystem::path& path ) const
			-> std::optional< std::string >;

		[[nodiscard]] auto contains( const std::filesystem::path& path ) const -> bool;

	private:
		std::map< std::filesystem::path, std::string, std::less<> > hashes_;
	};

}
