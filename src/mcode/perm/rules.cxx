#include "mcode/perm/rules.hxx"

#include "mcode/perm/permission.hxx"

#include "mcode/support/glob.hxx"

#include <algorithm>

namespace mcode::perm {

	namespace {

		// The basename of a canonical, forward-slashed path.
		[[nodiscard]] auto base_name( const std::string_view path ) -> std::string_view {
			const auto slash = path.find_last_of( '/' );

			return slash == std::string_view::npos
				? path
				: path.substr( slash + 1 );
		}

		// True when any path segment equals the name. Segment-wise, so a
		// sibling directory with the same prefix is not the credential one.
		[[nodiscard]] auto has_segment( const std::string_view path,
			const std::string_view name ) -> bool {
			const auto segments = support::glob_segments( path );

			return std::any_of( segments.begin( ), segments.end( ),
				[ name ]( const std::string_view segment ) { return segment == name; } );
		}

		// True when the basename matches the glob. Case-folded: Windows and
		// macOS file systems are case-insensitive, and `KEY.PEM` is the same
		// file as `key.pem`.
		[[nodiscard]] auto name_matches( const std::string_view path,
			const std::string_view pattern ) -> bool {
			const auto fold = []( const std::string_view text ) {
				auto lowered = std::string{ text };

				for ( auto& character : lowered ) {
					if ( character >= 'A' && character <= 'Z' ) {
						character = static_cast< char >( character - 'A' + 'a' );
					}
				}

				return lowered;
			};

			return support::wildcard_match( fold( pattern ), fold( base_name( path ) ) );
		}

	}

	auto rule_matches( const rule& candidate, const tool_class request_class,
		const std::string& request_resource, const std::string& request_tool_name )
		-> bool {
		if ( candidate.klass != request_class ) {
			return false;
		}

		// Exec rules match the canonical argv exactly, never a prefix: a
		// remembered `git status` must not authorize `git push`.
		if ( request_class == tool_class::exec ) {
			return candidate.pattern == request_resource;
		}

		// Path rules are globs over the canonical path. `*` within a segment,
		// `**` across segments; a trailing `/*` does not cross into deeper
		// directories unless the pattern says `**`.
		if ( request_class == tool_class::read || request_class == tool_class::write ) {
			return support::glob_match( candidate.pattern, request_resource );
		}

		// Tool-class rules (mcp, net, spawn) match the tool name exactly.
		return candidate.pattern == request_tool_name;
	}

	auto default_secret_deny( const std::string& canonical_path )
		-> std::optional< std::string > {
		const auto name = base_name( canonical_path );

		// `.env` and its common variants. `environment.md` is a different file
		// name and does not fire; the dot is what marks the hidden config.
		if ( name == ".env" || name.starts_with( ".env." ) ) {
			return "environment variable file";
		}

		// Key material, by suffix and by name. `id_rsa.pub` is a public key --
		// half of a pair whose secrecy lies entirely in the private half -- so
		// the rule names the private side and the public side stays readable.
		if ( name_matches( canonical_path, "*.pem" ) ||
			name_matches( canonical_path, "*.key" ) ) {
			return "key material";
		}

		if ( name == "id_rsa" || name.starts_with( "id_rsa." ) ) {
			return "key material";
		}

		// Credential directories. A file under the directory is a credential
		// store entry.
		if ( has_segment( canonical_path, ".aws" ) ) {
			return "credential directory";
		}

		return std::nullopt;
	}

}
