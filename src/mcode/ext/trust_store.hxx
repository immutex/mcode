#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::ext {

	enum class trust_scope {
		user,
		project,
	};

	enum class trust_decision {
		// No record for this extension: it is inert until a grant exists.
		ungranted,
		granted,
		denied,
	};

	struct trust_record {
		trust_scope scope = trust_scope::user;
		std::string name;
		std::int64_t api_version = 0;
		std::string tree_sha256;
		std::filesystem::path root;
	};

	// Line-oriented and tab-separated rather than TOML: this file is written and
	// read on a security path, and an exact field count fails closed on
	// corruption where a TOML parse would invite a defaulted field.
	class trust_store {
	public:
		// An absent store reads as empty, which means every project extension is
		// inert. That is the correct first-run default, not an error.
		trust_store( ) = default;

		[[nodiscard]] static auto default_path( ) -> result< std::filesystem::path >;

		// An unparsable line fails the whole store, so a corrupt file never reads as empty.
		[[nodiscard]] static auto load( const std::filesystem::path& path )
			-> result< trust_store >;

		[[nodiscard]] auto decide( const trust_scope scope, const std::string_view name,
			const std::int64_t api_version ) const -> trust_decision;

		[[nodiscard]] auto record_for( const trust_scope scope, std::string_view name ) const
			-> const trust_record*;

		[[nodiscard]] auto records( ) const noexcept -> const std::vector< trust_record >& {
			return records_;
		}

		// Temp file plus rename: a crash cannot truncate the store into an empty allow-list.
		auto grant( const trust_record& record ) -> status;
		auto revoke( const trust_scope scope, std::string_view name ) -> status;

		// Writes a record set to a path without going through an instance, so the
		// caller's copy is only replaced once the bytes are safely on disk.
		[[nodiscard]] static auto write_to( const std::filesystem::path& path,
			const std::vector< trust_record >& records ) -> status;

	private:
		auto save( ) const -> status;

		std::filesystem::path path_;
		std::vector< trust_record > records_;
	};

	[[nodiscard]] auto to_string( const trust_scope scope ) noexcept -> std::string_view;

}
