#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/fs/workspace.hxx"

namespace mcode::perm {

	// JSON, not TOML: config.toml is read-only here and appending to it risks clobbering it.

	inline constexpr std::int64_t STORE_VERSION = 1;

	enum class store_decision {
		allow,
		deny,
	};

	struct store_layer {
		std::map< std::string, store_decision, std::less<> > exec;
		std::map< std::string, store_decision, std::less<> > paths;
		std::map< std::string, store_decision, std::less<> > tools;
	};

	// single space between tokens, no quoting.
	[[nodiscard]] auto canonical_argv( const std::vector< std::string >& argv ) -> std::string;

	// absent when the path cannot be canonicalized, which would silently mismatch the entry.
	[[nodiscard]] auto canonical_path_key( const std::filesystem::path& path )
		-> std::optional< std::string >;

	// a missing file is the normal first-run case; unparsable JSON is an error, fail closed.
	class remember_store {
	public:
		explicit remember_store( std::filesystem::path file );

		[[nodiscard]] auto load( ) -> result< std::optional< store_layer > >;

		// atomic: sibling temp file plus rename, so a crash mid-write leaves the old contents.
		[[nodiscard]] auto save( const store_layer& additions ) -> status;

		[[nodiscard]] auto file( ) const noexcept -> const std::filesystem::path& {
			return file_;
		}

	private:
		std::filesystem::path file_;
	};

	// exec keys on the canonical argv, never a prefix; tool-class requests key on the tool name.
	enum class store_section {
		exec,
		paths,
		tools,
	};

	struct store_key {
		store_section section = store_section::tools;
		std::string key;
	};

	using store_map = std::map< std::string, store_decision, std::less<> >;

	[[nodiscard]] auto section_of( store_layer& layer, store_section section ) -> store_map&;
	[[nodiscard]] auto section_of( const store_layer& layer, store_section section )
		-> const store_map&;

	// not permission_request: permission.hxx includes this header, so it would be a cycle.
	struct request_identity {
		tool_class klass = tool_class::exec;
		std::string tool_name;
		std::string resource;
	};

	// a file under the workspace root is the project's: drop every allow it carries.
	[[nodiscard]] auto load_store_into( remember_store* store, const mcode::workspace& space,
		store_layer& into, std::vector< std::string >& warnings ) -> status;

	[[nodiscard]] auto store_key_for( const request_identity& request )
		-> std::optional< store_key >;

}

