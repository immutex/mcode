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

		// each line keeps the ending its ORIGINAL had, so a mixed-ending file stays mixed
		[[nodiscard]] auto splice_with_original_endings( const std::string_view original,
			const std::string_view normalised_after )
			-> std::string {
			const auto old_lines = split_keepings_endings( original );
			auto new_lines = split_keepings_endings( normalised_after );

			auto out = std::string{ };
			out.reserve( normalised_after.size( ) + new_lines.lines.size( ) );

			// agreed lines emit the original bytes; a replaced line takes the ending at that index
			auto old_index = std::size_t{ 0 };
			auto new_index = std::size_t{ 0 };

			while ( new_index < new_lines.lines.size( ) ) {
				auto new_line = std::string_view{ new_lines.lines[ new_index ] };
				auto new_body = new_line;
				auto new_ending = std::string_view{ };

				if ( new_body.size( ) >= 2 && new_body.substr( new_body.size( ) - 2 ) == "\r\n" ) {
					new_ending = new_body.substr( new_body.size( ) - 2 );
					new_body.remove_suffix( 2 );
				} else if ( !new_body.empty( ) && new_body.back( ) == '\n' ) {
					new_ending = new_body.substr( new_body.size( ) - 1 );
					new_body.remove_suffix( 1 );
				}

				if ( old_index < old_lines.lines.size( ) ) {
					const auto& original_line = old_lines.lines[ old_index ];
					auto original_body = std::string_view{ original_line };
					auto original_ending = std::string_view{ };

					if ( original_body.size( ) >= 2 &&
						original_body.substr( original_body.size( ) - 2 ) == "\r\n" ) {
						original_ending = original_body.substr( original_body.size( ) - 2 );
						original_body.remove_suffix( 2 );
					} else if ( !original_body.empty( ) && original_body.back( ) == '\n' ) {
						original_ending = original_body.substr( original_body.size( ) - 1 );
						original_body.remove_suffix( 1 );
					}

					if ( original_body == new_body ) {
						out.append( original_line );
						++old_index;
						++new_index;

						continue;
					}

					out.append( new_body );
					out.append( original_ending );
					++old_index;
					++new_index;

					continue;
				}

				out.append( new_body );
				out.append( new_ending );
				++new_index;
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

		updated = splice_with_original_endings( *original, updated );

		auto written = space.write_file( *path, updated, write_mode::overwrite );

		if ( !written ) {
			return error_result( "edit failed to write: " + written.error( ).msg,
				"the replacement was computed but the write failed; the file is unchanged", false );
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
