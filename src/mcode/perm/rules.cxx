#include "mcode/perm/rules.hxx"

#include "mcode/perm/permission.hxx"

#include "mcode/support/glob.hxx"

#include <algorithm>

namespace mcode::perm {

	namespace {

		[[nodiscard]] auto base_name( const std::string_view path ) -> std::string_view {
			const auto slash = path.find_last_of( '/' );

			return slash == std::string_view::npos
				? path
				: path.substr( slash + 1 );
		}

		// segment-wise, so a sibling directory sharing the prefix is not the credential one.
		[[nodiscard]] auto has_segment( const std::string_view path,
			const std::string_view name ) -> bool {
			const auto segments = support::glob_segments( path );

			return std::any_of( segments.begin( ), segments.end( ),
				[ name ]( const std::string_view segment ) { return segment == name; } );
		}

		// case-folded: Windows and macOS file systems are case-insensitive.
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

		if ( request_class == tool_class::exec ) {
			return candidate.pattern == request_resource;
		}

		// `*` matches within a segment, `**` across segments.
		if ( request_class == tool_class::read || request_class == tool_class::write ) {
			return support::glob_match( candidate.pattern, request_resource );
		}

		return candidate.pattern == request_tool_name;
	}

	auto default_secret_deny( const std::string& canonical_path )
		-> std::optional< std::string > {
		const auto name = base_name( canonical_path );

		if ( name == ".env" || name.starts_with( ".env." ) ) {
			return "environment variable file";
		}

		if ( name_matches( canonical_path, "*.pem" ) ||
			name_matches( canonical_path, "*.key" ) ) {
			return "key material";
		}

		if ( name == "id_rsa" || name.starts_with( "id_rsa." ) ) {
			return "key material";
		}

		if ( has_segment( canonical_path, ".aws" ) ) {
			return "credential directory";
		}

		return std::nullopt;
	}

}
