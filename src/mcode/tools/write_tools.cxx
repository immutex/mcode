#include "mcode/tools/file_tools.hxx"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"

namespace mcode::tools {

	namespace {

		// Files up to the write cap are legitimate edit targets, so the verifier
		// must hash raw bytes past read_file's 8 MiB whole-file cap — otherwise a
		// 9 MiB file reads fine and then every write is refused.
		inline constexpr std::uintmax_t VERIFY_READ_BYTES = MAX_WRITE_FILE_BYTES;

		[[nodiscard]] auto raw_content_hash( const std::filesystem::path& absolute )
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

		// The invariant check shared by write-overwrite and edit: the file must have
		// been read this session, and the hash recorded then must still match disk.
		[[nodiscard]] auto check_readable_state( session_reads& reads,
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

		[[nodiscard]] auto refuse_protected( const std::string_view path ) -> std::string {
			return error_result( "refusing to write " + std::string{ path },
				"paths under .mcode/ or .git/ are denied to tools; the harness writes its own "
				"state there and a tool write there would forge evidence or escape the trust boundary",
				false );
		}

		// The write decision, shared by write and edit. The engine resolves
		// it: a deny rule denies, the default set prompts for a path outside
		// the workspace (or denies headless), and an answer is honoured.
		// Protected paths keep their deny semantics through the engine's
		// floor, so a refusal here is still a refusal.
		[[nodiscard]] auto check_write_permission( tool_context& context,
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

		// Rebuilds the edited text so each line keeps the line ending its ORIGINAL
		// had. Lines are matched by index between the normalised before/after; a
		// line outside the replaced span is byte-identical, so copying its original
		// bytes (ending included) is exact, and a replaced line inherits the ending
		// of the original line at the same index. A mixed-ending file keeps its
		// mixture, and untouched lines are never rewritten.
		[[nodiscard]] auto splice_with_original_endings( const std::string_view original,
			const std::string_view normalised_after )
			-> std::string {
			const auto old_lines = split_keepings_endings( original );
			auto new_lines = split_keepings_endings( normalised_after );

			auto out = std::string{ };
			out.reserve( normalised_after.size( ) + new_lines.lines.size( ) );

			// Walk both line lists; where they agree in normalised content, emit
			// the ORIGINAL bytes verbatim, otherwise emit the new line with the
			// original's ending at that index (or LF when past the original's end).
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

		if ( resolved && space.is_protected( *resolved ) ) {
			return refuse_protected( *path );
		}

		if ( const auto denied = check_write_permission( context, *path ) ) {
			return *denied;
		}

		if ( !resolved ) {
			// The engine allowed a path outside the workspace. The workspace's
			// own writer refuses it, so the write goes to the canonical path
			// directly. Create-only: an overwrite outside the workspace has no
			// read-before-write invariant behind it, so it stays refused.
			auto candidate = std::filesystem::path{ space.root( ) } /
				std::filesystem::path{ *path };
			auto canonical = platform::canonicalize( candidate );

			if ( !canonical ) {
				return error_result( "cannot resolve " + *path + ": " + resolved.error( ).msg,
					"check the path; paths are relative to the workspace root", false );
			}

			auto error_code = std::error_code{ };
			const auto exists = std::filesystem::exists(
				platform::to_extended_path( *canonical ), error_code );

			if ( error_code ) {
				return error_result( "cannot check " + *path + ": " + error_code.message( ),
					"the path resolved but its existence could not be determined", false );
			}

			if ( exists ) {
				return error_result( "refusing to overwrite " + *path,
					"the file is outside the workspace, so the read-before-write invariant "
					"cannot protect it; delete or move it first, or write a new file",
					false );
			}

			if ( !canonical->parent_path( ).empty( ) ) {
				std::filesystem::create_directories(
					platform::to_extended_path( canonical->parent_path( ) ), error_code );

				if ( error_code ) {
					return error_result( "cannot create " +
							canonical->parent_path( ).string( ) + ": " + error_code.message( ),
						"check the parent directory is creatable", false );
				}
			}

			auto output = std::ofstream{ platform::to_extended_path( *canonical ),
				std::ios::binary | std::ios::trunc };

			if ( !output ) {
				return error_result( "cannot open " + canonical->string( ),
					"check the path is a file, not a directory, and is writable", false );
			}

			output.write( content->data( ), static_cast< std::streamsize >( content->size( ) ) );
			output.close( );

			if ( !output ) {
				return error_result( "failed to write " + canonical->string( ),
					"the file was opened but the write failed; check the disk and permissions",
					false );
			}

			auto out = std::string{ "{\"ok\":true,\"path\":\"" };
			json::append_escaped( out, *path );
			out += "\",\"bytes\":" + std::to_string( content->size( ) );
			out += ",\"mode\":\"create\",\"outside_workspace\":true}";

			return out;
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
			const auto invariant = check_readable_state( *context.reads, *resolved, *path );

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

		// Compare on a normalised copy so a CRLF file edited with an LF anchor
		// still matches; the file's own endings are preserved per line on write.
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

		// Build the replacement as normalised text, then splice it back into the
		// ORIGINAL byte stream at line granularity: each replaced line takes the
		// ending its original had, and lines outside the replaced span are never
		// touched. A mixed-ending file keeps its mixture.
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

		// Record the new hash so a second edit in the same turn does not read as
		// stale.
		context.reads->record( *resolved, written->content_hash );

		// The diff compares normalised text on both sides; `updated` carries the
		// file's original endings after the splice.
		const auto diff_after = normalise( updated );

		auto out = std::string{ "{\"ok\":true,\"path\":\"" };
		json::append_escaped( out, *path );
		out += "\",\"replacements\":" + std::to_string( occurrences );
		out += ",\"diff\":\"";
		json::append_escaped( out, unified_diff( normalised, diff_after, *path ) );
		out += "\"}";

		return truncate_result( out, context.run_id, space, "edit" );
	}

}
