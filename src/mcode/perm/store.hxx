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

	// The remember store: `permissions.json` beside the config, keyed by the
	// same four scopes the config uses. JSON, not TOML -- config.toml is
	// read-only in this tree, and appending to a file the user hand-edits
	// risks clobbering their comments. Written by us, for us, inspectable and
	// deletable.
	//
	// Shape:
	// {
	//   "version": 1,
	//   "exec":  { "git status": "allow" },
	//   "paths": { "C:/Users/u/notes/*": "allow" },
	//   "tools": { "mcp__github__create_issue": "allow" }
	// }
	//
	// Keys are canonical: exec argv joined with single spaces, paths canonical
	// and forward-slashed. `git   status` and `git status` are one entry.

	inline constexpr std::int64_t STORE_VERSION = 1;

	enum class store_decision {
		allow,
		deny,
	};

	// One scope's entries, parsed. A map per verb keeps the lookup flat.
	struct store_layer {
		std::map< std::string, store_decision, std::less<> > exec;
		std::map< std::string, store_decision, std::less<> > paths;
		std::map< std::string, store_decision, std::less<> > tools;
	};

	// Joins parsed argv the way the store keys it: single space between
	// tokens, no quoting. The canonical form of `git  status` and
	// `git status` is identical, which is what makes a remembered answer
	// match both spellings.
	[[nodiscard]] auto canonical_argv( const std::vector< std::string >& argv ) -> std::string;

	// Normalises a path for a store key: canonical, forward slashes. Absent
	// when the path cannot be canonicalized -- a key that is not the real
	// path would silently mismatch the entry it was remembered under.
	[[nodiscard]] auto canonical_path_key( const std::filesystem::path& path )
		-> std::optional< std::string >;

	// A store bound to one file. Missing file is the normal first-run case:
	// an empty layer, not an error. Unparsable JSON is an error, fail closed
	// -- a store that half-loads would re-prompt for everything or, worse,
	// honour only part of the user's denies.
	class remember_store {
	public:
		explicit remember_store( std::filesystem::path file );

		// Loads the file. The optional is absent when the file does not
		// exist; the error is a parse or read failure.
		[[nodiscard]] auto load( ) -> result< std::optional< store_layer > >;

		// Merges entries into the existing file content and rewrites it.
		// Atomic: sibling temp file plus rename, so a crash mid-write leaves
		// the previous contents intact rather than truncated.
		[[nodiscard]] auto save( const store_layer& additions ) -> status;

		// The file this store reads and writes.
		[[nodiscard]] auto file( ) const noexcept -> const std::filesystem::path& {
			return file_;
		}

	private:
		std::filesystem::path file_;
	};

	// Which section a request belongs to, and the key within it. One function
	// for both the write and the read: when the two disagreed, a remembered
	// answer was written to a section nothing ever looked in.
	//
	// Exec keys on the canonical argv, never a prefix -- a remembered
	// `git push` must not authorize `git push --force`. Everything else keys on
	// the tool name, because its resource is an opaque argument blob rather
	// than a stable identity.
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

	// The three fields the key depends on. A separate struct rather than
	// `permission_request`, which lives in permission.hxx and already includes
	// this header -- taking it here would be a cycle.
	struct request_identity {
		tool_class klass = tool_class::exec;
		std::string tool_name;
		std::string resource;
	};

	// Loads the store and drops every project-scope allow with a warning: a
	// cloned repository must not be able to grant itself permissions. The
	// store's origin decides -- a file under the workspace root is the
	// project's. Denies survive.
	[[nodiscard]] auto load_store_into( remember_store* store, const mcode::workspace& space,
		store_layer& into, std::vector< std::string >& warnings ) -> status;

	// Nothing to key on: an unparsable command has no canonical form, and a
	// path is never auto-persisted, so the user's own `paths` globs stay theirs.
	[[nodiscard]] auto store_key_for( const request_identity& request )
		-> std::optional< store_key >;

}

