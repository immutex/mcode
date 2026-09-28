#include "mcode/tools/file_tools.hxx"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"

namespace mcode::tools {

	namespace {

		// The invariant check shared by write-overwrite and edit: the file must have
		// been read this session, and the hash recorded then must still match disk.
		[[nodiscard]] auto check_readable_state( const workspace& space, session_reads& reads,
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

			auto current = space.content_hash( std::string{ path } );

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

		[[nodiscard]] auto refuse_protected( const std::string_view path ) -> std::string {
			return error_result( "refusing to write " + std::string{ path },
				"paths under .mcode/ or .git/ are denied to tools; the harness writes its own "
				"state there and a tool write there would forge evidence or escape the trust boundary",
				false );
		}

		// Strips CR for comparison while keeping LF structure; the file's own
		// endings are restored before writing.
		[[nodiscard]] auto normalise( std::string_view text ) -> std::string {
			auto out = std::string{ text };
			out.erase( std::remove( out.begin( ), out.end( ), '\r' ), out.end( ) );

			return out;
		}

		// One diff line: the kept ending is stripped so the diff can add its own.
		[[nodiscard]] auto diff_line( std::string_view line ) -> std::string {
			auto out = normalise( line );

			if ( !out.empty( ) && out.back( ) == '\n' ) {
				out.pop_back( );
			}

			return out;
		}

		// Splits content into lines keeping line endings attached to each line, so
		// rejoining is exact and CRLF survives the round trip.
		struct split_lines {
			std::vector< std::string > lines;
		};

		[[nodiscard]] auto split_keepings_endings( std::string_view text ) -> split_lines {
			auto out = split_lines{ };
			auto start = std::size_t{ 0 };

			while ( start < text.size( ) ) {
				const auto newline = text.find( '\n', start );
				const auto end = ( newline == std::string_view::npos ) ? text.size( ) : newline + 1;

				out.lines.push_back( std::string{ text.substr( start, end - start ) } );

				if ( newline == std::string_view::npos ) {
					break;
				}

				start = end;
			}

			return out;
		}

		// A bounded unified diff of the changed region, not the whole file.
		[[nodiscard]] auto unified_diff( const std::string_view before,
			const std::string_view after, const std::string_view path ) -> std::string {
			auto old_lines = split_keepings_endings( before );
			auto new_lines = split_keepings_endings( after );

			auto diff = std::string{ "--- a/" };
			diff.append( path );
			diff += "\n+++ b/";
			diff.append( path );
			diff += "\n";

			// The anchor is one contiguous replacement: find where the two texts
			// first differ and where they last agree, and show that window only.
			auto first_difference = std::size_t{ 0 };

			while ( first_difference < old_lines.lines.size( ) &&
				first_difference < new_lines.lines.size( ) &&
				old_lines.lines[ first_difference ] == new_lines.lines[ first_difference ] ) {
				++first_difference;
			}

			auto old_tail = old_lines.lines.size( );
			auto new_tail = new_lines.lines.size( );

			while ( old_tail > first_difference && new_tail > first_difference &&
				old_lines.lines[ old_tail - 1 ] == new_lines.lines[ new_tail - 1 ] ) {
				--old_tail;
				--new_tail;
			}

			constexpr std::size_t DIFF_CONTEXT_LINES = 3;
			const auto window_start = first_difference > DIFF_CONTEXT_LINES
				? first_difference - DIFF_CONTEXT_LINES
				: std::size_t{ 0 };
			const auto window_end_old = std::min( old_tail + DIFF_CONTEXT_LINES, old_lines.lines.size( ) );
			const auto window_end_new = std::min( new_tail + DIFF_CONTEXT_LINES, new_lines.lines.size( ) );

			diff += "@@ -" + std::to_string( window_start + 1 ) + "," +
				std::to_string( window_end_old - window_start ) + " +" +
				std::to_string( window_start + 1 ) + "," +
				std::to_string( window_end_new - window_start ) + " @@\n";

			for ( auto index = window_start; index < window_end_old; ++index ) {
				diff += '-';
				diff += diff_line( old_lines.lines[ index ] );
				diff += '\n';
			}

			for ( auto index = window_start; index < window_end_new; ++index ) {
				diff += '+';
				diff += diff_line( new_lines.lines[ index ] );
				diff += '\n';
			}

			return diff;
		}

	}

	auto handle_write( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto path = args.string_field( "path" );

		if ( !path || path->empty( ) ) {
			return error_result( "missing required argument: path",
				"pass the file to create or overwrite, relative to the workspace root", false );
		}

		const auto content = args.string_field( "content" );

		if ( !content ) {
			return error_result( "missing required argument: content",
				"pass the complete new file content; an empty file is content: \"\"", false );
		}

		auto& space = *context.space;
		auto resolved = space.resolve( *path );

		if ( !resolved ) {
			return error_result( "cannot resolve " + *path + ": " + resolved.error( ).msg,
				"the path must stay inside the workspace; paths are relative to the root", false );
		}

		if ( space.is_protected( *resolved ) ) {
			return refuse_protected( *path );
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( *resolved ),
			error_code );

		if ( error_code ) {
			return error_result( "cannot check " + *path + ": " + error_code.message( ),
				"the path resolved but its existence could not be determined", false );
		}

		auto receipt = std::optional< write_receipt >{ };

		if ( !exists ) {
			auto created = space.write_file( *path, *content, write_mode::create );

			if ( !created ) {
				return error_result( "write failed: " + created.error( ).msg,
					"check the parent directory exists and the path is a file, not a directory",
					false );
			}

			receipt = *created;
		} else {
			const auto invariant = check_readable_state( space, *context.reads, *resolved, *path );

			if ( !invariant ) {
				return std::unexpected( invariant.error( ) );
			}

			auto replaced = space.write_file( *path, *content, write_mode::overwrite );

			if ( !replaced ) {
				return error_result( "write failed: " + replaced.error( ).msg,
					"the file exists and was read; a failure here is a filesystem error, not a policy one",
					false );
			}

			receipt = *replaced;
		}

		context.reads->record( *resolved, receipt->content_hash );

		auto out = std::string{ "{\"ok\":true,\"path\":\"" };
		json::append_escaped( out, *path );
		out += "\",\"bytes\":" + std::to_string( receipt->bytes_written );
		out += ",\"mode\":\"" + std::string{ exists ? "overwrite" : "create" } + "\"}";

		return out;
	}

	auto handle_edit( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto path = args.string_field( "path" );

		if ( !path || path->empty( ) ) {
			return error_result( "missing required argument: path",
				"pass the file to edit, relative to the workspace root; it must have been read this session",
				false );
		}

		const auto old_string = args.string_field( "old_string" );

		if ( !old_string ) {
			return error_result( "missing required argument: old_string",
				"pass the exact text to replace; an empty old_string is a create, not an edit -- use write for that",
				false );
		}

		if ( old_string->empty( ) ) {
			return error_result( "old_string is empty",
				"an empty anchor would replace nothing; to create a file use write, to insert use a non-empty anchor around the insertion point",
				false );
		}

		const auto new_string = args.string_field( "new_string" );

		if ( !new_string ) {
			return error_result( "missing required argument: new_string",
				"pass the replacement text; to delete, pass an empty string", false );
		}

		const auto replace_all = args.bool_field( "replace_all" ).value_or( false );

		auto& space = *context.space;
		auto resolved = space.resolve( *path );

		if ( !resolved ) {
			return error_result( "cannot resolve " + *path + ": " + resolved.error( ).msg,
				"the path must stay inside the workspace", false );
		}

		if ( space.is_protected( *resolved ) ) {
			return refuse_protected( *path );
		}

		auto error_code = std::error_code{ };

		if ( !std::filesystem::exists( platform::to_extended_path( *resolved ), error_code ) ||
			error_code ) {
			return error_result( "file not found: " + *path,
				"read the file first; edit works on files that exist", false );
		}

		const auto invariant = check_readable_state( space, *context.reads, *resolved, *path );

		if ( !invariant ) {
			return std::unexpected( invariant.error( ) );
		}

		auto original = space.read_file( *path );

		if ( !original ) {
			return error_result( "cannot read " + *path + ": " + original.error( ).msg,
				"the file exists but could not be read; check it is not binary", false );
		}

		// Compare on a normalised copy so a CRLF file edited with an LF anchor
		// still matches; the file's own endings are preserved on write.
		const auto normalised = normalise( *original );
		const auto anchor = normalise( *old_string );
		const auto replacement = normalise( *new_string );

		auto occurrences = std::size_t{ 0 };
		auto positions = std::vector< std::size_t >{ };
		auto search = std::size_t{ 0 };

		while ( ( search = normalised.find( anchor, search ) ) != std::string::npos ) {
			++occurrences;
			positions.push_back( search );
			++search;
		}

		if ( occurrences == 0 ) {
			return error_result( "old_string not found in " + *path,
				"the file may have changed since you read it -- re-read it; otherwise check the anchor matches exactly, including whitespace",
				false );
		}

		if ( occurrences > 1 && !replace_all ) {
			return error_result( "old_string appears " + std::to_string( occurrences ) +
					" times in " + *path,
				"include more surrounding context in old_string to disambiguate, or set replace_all to replace every occurrence",
				false );
		}

		auto updated = std::string{ };

		if ( occurrences == 1 ) {
			updated = normalised.substr( 0, positions[ 0 ] ) + replacement +
				normalised.substr( positions[ 0 ] + anchor.size( ) );
		} else {
			auto cursor = std::size_t{ 0 };

			for ( const auto position : positions ) {
				updated += normalised.substr( cursor, position - cursor );
				updated += replacement;
				cursor = position + anchor.size( );
			}

			updated += normalised.substr( cursor );
		}

		// Restore the file's own line endings: every LF that a CRLF file carried
		// comes back with its CR.
		const auto was_crlf = original->find( "\r\n" ) != std::string::npos;

		if ( was_crlf ) {
			auto restored = std::string{ };
			restored.reserve( updated.size( ) + positions.size( ) );

			for ( const auto character : updated ) {
				if ( character == '\n' ) {
					restored += "\r\n";
				} else {
					restored += character;
				}
			}

			updated = std::move( restored );
		}

		auto written = space.write_file( *path, updated, write_mode::overwrite );

		if ( !written ) {
			return error_result( "edit failed to write: " + written.error( ).msg,
				"the replacement was computed but the write failed; the file is unchanged", false );
		}

		// Record the new hash so a second edit in the same turn does not read as
		// stale.
		context.reads->record( *resolved, written->content_hash );

		// The diff compares the pre-restore normalised text; `updated` has already
		// had the file's CRLF endings put back at this point.
		const auto diff_after = was_crlf ? normalise( updated ) : updated;

		auto out = std::string{ "{\"ok\":true,\"path\":\"" };
		json::append_escaped( out, *path );
		out += "\",\"replacements\":" + std::to_string( occurrences );
		out += ",\"diff\":\"";
		json::append_escaped( out, unified_diff( normalised, diff_after, *path ) );
		out += "\"}";

		return truncate_result( out, context.run_id, space, "edit" );
	}

}
