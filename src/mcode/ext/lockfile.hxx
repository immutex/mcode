#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::ext {

	struct lock_entry {
		std::string name;
		std::string source;
		std::string ref;
		std::string commit;
		std::string commit_format;
		std::string tree_sha256;
		std::int64_t api_version = 0;
		std::vector< std::string > permissions;
	};

	// The install-time record: the trust store is the load-time authority, this file is what
	// a team commits so two machines resolve the same commit. `commit` and `tree_sha256`
	// are both recorded because they can disagree: an EOL filter makes the working tree
	// differ from the committed blob while the commit id stays valid.
	class lockfile {
	public:
		[[nodiscard]] static auto project_path( const std::filesystem::path& workspace )
			-> std::filesystem::path;

		[[nodiscard]] static auto user_path( ) -> result< std::filesystem::path >;

		[[nodiscard]] static auto load( const std::filesystem::path& path )
			-> result< lockfile >;

		[[nodiscard]] auto find( std::string_view name ) const -> const lock_entry*;
		[[nodiscard]] auto entries( ) const noexcept -> const std::vector< lock_entry >& {
			return entries_;
		}

		auto put( const lock_entry& entry ) -> void;
		auto save( ) const -> status;

	private:
		std::filesystem::path path_;
		std::vector< lock_entry > entries_;
	};

	// The host and path of a `git:owner/repo` coordinate; a full URL is refused so
	// a lockfile cannot be made to fetch from an arbitrary endpoint.
	[[nodiscard]] auto parse_git_coordinate( std::string_view coordinate, std::string_view ref,
		std::string& url_out, std::string& ref_out ) -> status;

}
