#include "mcode/tools/write_tools_common.hxx"

#include <algorithm>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"

namespace mcode::tools {

	namespace {

		[[nodiscard]] auto diff_line( std::string_view line ) -> std::string {
			auto out = normalise( line );

			if ( !out.empty( ) && out.back( ) == '\n' ) {
				out.pop_back( );
			}

			return out;
		}

		// endings stay attached to their line, so rejoining is exact
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

		// the body of a line, with its ending removed
		[[nodiscard]] auto line_body( const std::string_view line ) -> std::string_view {
			if ( line.size( ) >= 2 && line.substr( line.size( ) - 2 ) == "\r\n" ) {
				return line.substr( 0, line.size( ) - 2 );
			}

			if ( !line.empty( ) && line.back( ) == '\n' ) {
				return line.substr( 0, line.size( ) - 1 );
			}

			return line;
		}

		[[nodiscard]] auto line_ending( const std::string_view line ) -> std::string_view {
			if ( line.size( ) >= 2 && line.substr( line.size( ) - 2 ) == "\r\n" ) {
				return line.substr( line.size( ) - 2 );
			}

			if ( !line.empty( ) && line.back( ) == '\n' ) {
				return line.substr( line.size( ) - 1 );
			}

			return { };
		}

		// The endings are preserved around the edited region: the unchanged lines
		// before and after it are emitted byte for byte, and a replaced line takes
		// the ending of the line it replaced, so a CRLF file stays CRLF and an
		// unterminated final line stays unterminated.
		//
		// The region is found by common prefix and suffix, NOT by walking the two
		// line lists in lockstep by index. A replacement that changes the line
		// count breaks the index correspondence: with `a\nb\nc\nd`, replacing `b`
		// with `B1\nB2` left `c` and `d` merged into `cd`, because `c` was spliced
		// against `d`'s (empty, unterminated) ending. The edit still reported
		// success, and the read-back hash compares the written bytes against
		// themselves, so nothing downstream caught it.
		[[nodiscard]] auto splice_with_original_endings( const std::string_view original,
			const std::string_view normalised_after )
			-> std::string {
			const auto old_lines = split_keepings_endings( original );
			const auto new_lines = split_keepings_endings( normalised_after );

			const auto& old = old_lines.lines;
			const auto& updated = new_lines.lines;

			// The unchanged head and tail, compared on bodies so a line whose
			// ending the rewrite normalised still counts as unchanged.
			auto prefix = std::size_t{ 0 };

			while ( prefix < old.size( ) && prefix < updated.size( ) &&
				diff_line( old[ prefix ] ) == diff_line( updated[ prefix ] ) ) {
				++prefix;
			}

			auto suffix = std::size_t{ 0 };

			while ( suffix < old.size( ) - prefix && suffix < updated.size( ) - prefix &&
				diff_line( old[ old.size( ) - 1 - suffix ] ) ==
					diff_line( updated[ updated.size( ) - 1 - suffix ] ) ) {
				++suffix;
			}

			const auto old_middle = old.size( ) - prefix - suffix;
			const auto new_middle = updated.size( ) - prefix - suffix;

			auto out = std::string{ };
			out.reserve( normalised_after.size( ) + updated.size( ) );

			for ( auto index = std::size_t{ 0 }; index < prefix; ++index ) {
				out.append( old[ index ] );
			}

			if ( new_middle > 0 ) {
				// The ending the replacement region used, so inserted lines match
				// the file rather than whatever the edit's own text carried.
				auto style_ending = std::string_view{ };

				if ( old_middle > 0 ) {
					style_ending = line_ending( old[ prefix ] );

					if ( style_ending.empty( ) ) {
						style_ending = "\n";
					}
				}

				// The last inserted line adopts the region's own final ending, so a
				// replaced unterminated last line stays unterminated.
				auto tail_ending = std::string_view{ };

				if ( old_middle > 0 ) {
					tail_ending = line_ending( old[ prefix + old_middle - 1 ] );
				}

				for ( auto index = std::size_t{ 0 }; index < new_middle; ++index ) {
					const auto& line = updated[ prefix + index ];

					out.append( line_body( line ) );

					if ( index + 1 < new_middle ) {
						out.append( style_ending );
					} else if ( old_middle > 0 ) {
						out.append( tail_ending );
					} else {
						// A pure insertion has no replaced line to take an ending
						// from, so the text's own ending is the only source.
						out.append( line_ending( line ) );
					}
				}
			}

			for ( auto index = old.size( ) - suffix; index < old.size( ); ++index ) {
				out.append( old[ index ] );
			}

			return out;
		}

		[[nodiscard]] auto make_unified_diff( const std::string_view before,
			const std::string_view after, const std::string_view path ) -> std::string {
			auto old_lines = split_keepings_endings( before );
			auto new_lines = split_keepings_endings( after );

			auto diff = std::string{ "--- a/" };
			diff.append( path );
			diff += "\n+++ b/";
			diff.append( path );
			diff += "\n";

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
			const auto window_end_old = std::min( old_tail + DIFF_CONTEXT_LINES,
				old_lines.lines.size( ) );
			const auto window_end_new = std::min( new_tail + DIFF_CONTEXT_LINES,
				new_lines.lines.size( ) );

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

	auto handle_edit( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto path = args.string_field( "path" );

		if ( !path || path->empty( ) ) {
			return error_result( "missing required argument: path",
				"pass the file to edit, relative to the workspace root; "
				"it must have been read this session",
				false );
		}

		const auto old_string = args.string_field( "old_string" );

		if ( !old_string ) {
			return error_result( "missing required argument: old_string",
				"pass the exact text to replace; an empty old_string is a create, "
				"not an edit -- use write for that",
				false );
		}

		if ( old_string->empty( ) ) {
			return error_result( "old_string is empty",
				"an empty anchor would replace nothing; to create a file use write, "
				"to insert use a non-empty anchor around the insertion point",
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

		if ( resolved && space.is_protected( *resolved ) ) {
			return refuse_protected( *path );
		}

		if ( const auto denied = check_write_permission( context, *path ) ) {
			return *denied;
		}

		if ( !resolved ) {
			return error_result( "cannot edit " + *path,
				"the path is outside the workspace; an edit needs the read-before-write "
				"invariant, which only workspace reads provide -- write the file instead",
				false );
		}

		auto error_code = std::error_code{ };

		if ( !std::filesystem::exists( platform::to_extended_path( *resolved ), error_code ) ||
			error_code ) {
			return error_result( "file not found: " + *path,
				"read the file first; edit works on files that exist", false );
		}

		const auto invariant = check_readable_state( *context.reads, *resolved, *path );

		if ( !invariant ) {
			return std::unexpected( invariant.error( ) );
		}

		auto original = space.read_file( *path );

		if ( !original ) {
			return error_result( "cannot read " + *path + ": " + original.error( ).msg,
				"the file exists but could not be read; check it is not binary", false );
		}

		// compare normalised, so an LF anchor matches a CRLF file; endings are restored on write
		const auto normalised = normalise( *original );
		const auto anchor = normalise( *old_string );
		const auto replacement = normalise( *new_string );

		// an anchor of only line endings normalises to nothing, which would scan forever
		if ( anchor.empty( ) ) {
			return error_result( "old_string is empty once line endings are normalised",
				"pass a non-empty anchor; to create a file use write, "
				"to insert use a non-empty anchor",
				false );
		}

		auto positions = std::vector< std::size_t >{ };
		auto search = std::size_t{ 0 };

		// advancing by the anchor keeps the matches disjoint, so a
		// self-overlapping anchor counts once
		while ( ( search = normalised.find( anchor, search ) ) != std::string::npos ) {
			positions.push_back( search );
			search += anchor.size( );
		}

		const auto occurrences = positions.size( );

		if ( occurrences == 0 ) {
			return error_result( "old_string not found in " + *path,
				"the file may have changed since you read it -- re-read it; "
				"otherwise check the anchor matches exactly, including whitespace",
				false );
		}

		if ( occurrences > 1 && !replace_all ) {
			return error_result( "old_string appears " + std::to_string( occurrences ) +
					" times in " + *path,
				"include more surrounding context in old_string to disambiguate, "
				"or set replace_all to replace every occurrence",
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

		updated = splice_with_original_endings( *original, updated );

		auto written = space.write_file( *path, updated, write_mode::overwrite );

		if ( !written ) {
			return error_result( "edit failed to write: " + written.error( ).msg,
				"the replacement was computed but the write failed; the file is unchanged", false );
		}

		// read the file back: the hash must describe the disk bytes, not the buffer
		// handed to the writer
		const auto persisted = raw_content_hash( *resolved );

		if ( !persisted || *persisted != written->content_hash ) {
			return error_result( "edit wrote " + *path + " but it did not read back as written",
				"re-read the file and retry; the write and the read disagree", false );
		}

		// so a second edit in the same turn does not read as stale
		context.reads->record( *resolved, written->content_hash );

		const auto diff_after = normalise( updated );

		auto out = std::string{ "{\"ok\":true,\"path\":\"" };
		json::append_escaped( out, *path );
		out += "\",\"replacements\":" + std::to_string( occurrences );
		out += ",\"diff\":\"";
		json::append_escaped( out, make_unified_diff( normalised, diff_after, *path ) );
		out += "\"}";

		return truncate_result( out, context.run_id, space, "edit" );
	}

}
