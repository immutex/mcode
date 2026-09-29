#include "mcode/tools/session_reads.hxx"

namespace mcode::tools {

	auto session_reads::record( const std::filesystem::path& path, std::string content_hash ) -> void {
		hashes_[ path ] = std::move( content_hash );
	}

	auto session_reads::find( const std::filesystem::path& path ) const
		-> std::optional< std::string > {
		const auto found = hashes_.find( path );

		if ( found == hashes_.end( ) ) {
			return std::nullopt;
		}

		return found->second;
	}

	auto session_reads::contains( const std::filesystem::path& path ) const -> bool {
		return hashes_.find( path ) != hashes_.end( );
	}

}
