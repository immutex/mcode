// The helpers shared by the write and edit tools: the read-before-write
// invariant, the permission decision, and the protected-path refusal.

#include "mcode/tools/write_tools_common.hxx"

#include <algorithm>
#include <fstream>

#include "mcode/tools/errors.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode::tools {

	auto raw_content_hash( const std::filesystem::path& absolute )
		-> result< std::string > {
		auto input = std::ifstream{ platform::to_extended_path( absolute ), std::ios::binary };

		if ( !input ) {
			return std::unexpected( fail( errc::io, "cannot open " + absolute.string( ) ) );
		}

		auto content = std::string{ };
		content.resize( static_cast< std::size_t >( VERIFY_READ_BYTES ) + 1 );

		input.read( content.data( ), static_cast< std::streamsize >( VERIFY_READ_BYTES + 1 ) );
		content.resize( static_cast< std::size_t >( input.gcount( ) ) );

		if ( content.size( ) > VERIFY_READ_BYTES ) {
			return std::unexpected( fail( errc::io,
				"file exceeds the " + std::to_string( VERIFY_READ_BYTES ) +
					"-byte write cap" ) );
		}

		return hash_bytes( content );
	}

	auto check_readable_state( session_reads& reads,
		const std::filesystem::path& absolute, const std::string_view path )
		-> result< std::string > {
		const auto recorded = reads.find( absolute );

		if ( !recorded ) {
			return std::unexpected( fail( errc::tool_failed,
				error_result( "refusing to modify " + std::string{ path } +
						" without reading it first",
					"call read on this path first; the read-before-write invariant keeps edits "
					"anchored to content you have seen",
					false ) ) );
		}

		auto current = raw_content_hash( absolute );

		if ( !current ) {
			return std::unexpected( fail( errc::tool_failed,
				error_result( "cannot verify " + std::string{ path } + " is unchanged",
					"the file could not be hashed; check it still exists and is readable", false ) ) );
		}

		if ( *current != *recorded ) {
			return std::unexpected( fail( errc::tool_failed,
				error_result( "stale read: " + std::string{ path } +
						" changed since it was last read",
					"re-read the file to pick up its current content (hash " + *current +
						"), then re-apply the edit",
					false ) ) );
		}

		return *recorded;
	}

	auto refuse_protected( const std::string_view path ) -> std::string {
		return error_result( "refusing to write " + std::string{ path },
			"paths under .mcode/ or .git/ are denied to tools; the harness writes its own "
			"state there and a tool write there would forge evidence or escape the trust boundary",
			false );
	}

	auto normalise( std::string_view text ) -> std::string {
		auto out = std::string{ text };
		out.erase( std::remove( out.begin( ), out.end( ), '\r' ), out.end( ) );

		return out;
	}

	auto check_write_permission( tool_context& context,
		const std::string_view path ) -> std::optional< std::string > {
		auto& space = *context.space;
		auto resolved = space.resolve( path );

		auto request = perm::permission_request{ };
		request.tool_name = "write";
		request.klass = tool_class::write;

		if ( resolved ) {
			request.resource = resolved->generic_string( );
		} else {
			// Outside the workspace: the engine still decides, on the
			// canonical spelling of the named path. An unresolvable path
			// stays a hard refusal -- there is nothing to judge.
			auto candidate = std::filesystem::path{ space.root( ) } /
				std::filesystem::path{ path };
			auto canonical = platform::canonicalize( candidate );

			if ( !canonical ) {
				return error_result( "cannot resolve " + std::string{ path } + ": " +
						resolved.error( ).msg,
					"check the path; paths are relative to the workspace root", false );
			}

			request.resource = canonical->generic_string( );
		}

		if ( context.permissions->decide( request ) == perm::permission_decision::allow ) {
			return std::nullopt;
		}

		const auto& verdict = context.permissions->last_verdict( );

		return error_result(
			"write denied by the permission engine: " + std::string{ path } +
				( verdict.reason.empty( ) ? std::string{ } : " (" + verdict.reason + ")" ),
			verdict.matched.scope == "floor"
				? "this path is protected; no flag or config overrides it"
				: "the path is outside the workspace; approve it interactively or add an allow rule",
			false );
	}

}
