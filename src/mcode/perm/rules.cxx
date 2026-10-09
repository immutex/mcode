#include "mcode/perm/rules.hxx"

#include "mcode/perm/permission.hxx"

#include "mcode/support/glob.hxx"
#include "mcode/support/text.hxx"

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
			const auto wanted = mcode::text::ascii_lower( name );

			return std::any_of( segments.begin( ), segments.end( ),
				[ &wanted ]( const std::string_view segment ) {
					return mcode::text::ascii_lower( segment ) == wanted;
				} );
		}

		[[nodiscard]] auto name_matches( const std::string_view path,
			const std::string_view pattern ) -> bool {
			return support::wildcard_match( mcode::text::ascii_lower( pattern ),
				mcode::text::ascii_lower( base_name( path ) ) );
		}

		// A folded whole-name test: the secret names below are matched as
		// literal names rather than patterns, so `wildcard_match` would give
		// their `*` and `?` a meaning they do not have.
		[[nodiscard]] auto name_is( const std::string_view path,
			const std::string_view name ) -> bool {
			return mcode::text::ascii_lower( base_name( path ) ) == mcode::text::ascii_lower( name );
		}

		[[nodiscard]] auto name_starts_with( const std::string_view path,
			const std::string_view prefix ) -> bool {
			return mcode::text::ascii_lower( base_name( path ) ).starts_with(
				mcode::text::ascii_lower( prefix ) );
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
		if ( name_is( canonical_path, ".env" ) ||
			name_starts_with( canonical_path, ".env." ) ) {
			return "environment variable file";
		}

		if ( name_matches( canonical_path, "*.pem" ) ||
			name_matches( canonical_path, "*.key" ) ) {
			return "key material";
		}

		if ( name_is( canonical_path, "id_rsa" ) ||
			name_starts_with( canonical_path, "id_rsa." ) ) {
			return "key material";
		}

		if ( has_segment( canonical_path, ".aws" ) ) {
			return "credential directory";
		}

		return std::nullopt;
	}

}
