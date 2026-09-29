#include "mcode/tools/file_tools.hxx"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools {

	namespace {

		// Binary content is refused on bytes before any decoding, so a probe of the
		// head is what decides -- not the decoded text.
		inline constexpr std::size_t BINARY_PROBE_BYTES = 8192;

		// A line longer than this is machine-generated output, not something to
		// edit through a viewport.
		inline constexpr std::size_t MINIFIED_LINE_BYTES = 5u * 1024u;

		// Files at or above this size get the large-file stub treatment even though
		// a window is served: the model should navigate deliberately, not page
		// through blindly. Same threshold the docs give for the >1 MiB row.
		inline constexpr std::uintmax_t LARGE_FILE_BYTES = 1u * 1024u * 1024u;

		// Window serving reads at most this many bytes per call, which is what
		// keeps a 9 MiB file readable a window at a time. Well above any 1000-line
		// window of normal source, far below the write cap.
		inline constexpr std::uintmax_t WINDOW_READ_BYTES = 16u * 1024u * 1024u;

		// The closest-existing-path search bounds itself to the parent directory
		// and this many candidates, so typo recovery never walks the tree.
		inline constexpr std::size_t SUGGESTION_LIMIT = 12;

		inline constexpr std::size_t MAX_READ_LINES = 1000;

		struct binary_extensions {
			static constexpr auto NAMES = std::array< std::string_view, 24 >{
				".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".pdf", ".zip",
				".gz", ".tar", ".7z", ".exe", ".dll", ".lib", ".obj", ".o",
				".a", ".so", ".dylib", ".pdb", ".bin", ".class", ".jar", ".woff2",
			};

			[[nodiscard]] static auto contains( const std::string_view extension ) noexcept -> bool {
				for ( const auto name : NAMES ) {
					if ( extension == name ) {
						return true;
					}
				}

				return false;
			}
		};

		[[nodiscard]] auto extension_of( const std::filesystem::path& path ) -> std::string {
			auto text = path.extension( ).string( );

			for ( auto& character : text ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return text;
		}

		// Reads up to `max_bytes` from an absolute path without the whole-file cap.
		// The tool layer's window reader: `workspace::read_file` refuses past 8 MiB,
		// which is correct for whole-file consumers and wrong for a windowed read
		// of a file the model may legitimately edit (the write cap is 10 MiB).
		[[nodiscard]] auto read_window_bytes( const std::filesystem::path& absolute,
			const std::uintmax_t max_bytes ) -> result< std::string > {
			auto input = std::ifstream{ platform::to_extended_path( absolute ), std::ios::binary };

			if ( !input ) {
				return std::unexpected( fail( errc::io, "cannot open " + absolute.string( ) ) );
			}

			auto content = std::string{ };
			content.resize( static_cast< std::size_t >( max_bytes ) + 1 );

			input.read( content.data( ), static_cast< std::streamsize >( max_bytes + 1 ) );
			content.resize( static_cast< std::size_t >( input.gcount( ) ) );

			if ( content.size( ) > max_bytes ) {
				return std::unexpected( fail( errc::io,
					"window read exceeded its " + std::to_string( max_bytes ) + "-byte bound" ) );
			}

			return content;
		}

		// The closest existing sibling in the same directory, by a bounded
		// similarity heuristic on the file name. One directory listing, one pass.
		[[nodiscard]] auto closest_existing( const workspace& space,
			const std::filesystem::path& missing ) -> std::optional< std::string > {
			auto error_code = std::error_code{ };
			const auto parent = missing.parent_path( );

			if ( !std::filesystem::is_directory( platform::to_extended_path( parent ), error_code ) ||
				error_code ) {
				return std::nullopt;
			}

			const auto wanted = missing.filename( ).string( );
			auto best_distance = std::size_t{ 0 };
			auto best_name = std::optional< std::string >{ };
			auto candidates = std::size_t{ 0 };

			for ( const auto& entry :
				std::filesystem::directory_iterator{ platform::to_extended_path( parent ),
					std::filesystem::directory_options::skip_permission_denied, error_code } ) {
				if ( error_code ) {
					break;
				}

				if ( ++candidates > SUGGESTION_LIMIT * 16 ) {
					break;
				}

				const auto name = entry.path( ).filename( ).string( );

				if ( name == wanted ) {
					continue;
				}

				// Count matching leading characters and require the name to stay
				// close in length; no dynamic programming.
				const auto shared = std::min( name.size( ), wanted.size( ) );
				auto common = std::size_t{ 0 };

				while ( common < shared && name[ common ] == wanted[ common ] ) {
					++common;
				}

				const auto length_gap = name.size( ) > wanted.size( )
					? name.size( ) - wanted.size( )
					: wanted.size( ) - name.size( );

				if ( common < wanted.size( ) / 2 || length_gap > wanted.size( ) / 2 + 2 ) {
					continue;
				}

				const auto distance = ( wanted.size( ) - common ) + length_gap;

				if ( !best_name || distance < best_distance ) {
					best_distance = distance;
					best_name = space.display_path( entry.path( ) );
				}
			}

			return best_name;
		}

		// The read window itself, shared by the normal and large-file paths. Returns
		// the rendered JSON result.
		[[nodiscard]] auto render_window( const std::filesystem::path& absolute,
			const std::string_view relative, const std::size_t offset, const std::size_t limit,
			const std::string& content, const std::uintmax_t file_size,
			const bool size_is_complete, tool_context& context ) -> result< std::string > {
			const auto safe = text::sanitize_utf8( content );

			auto lines = std::vector< std::string_view >{ };
			auto start = std::size_t{ 0 };
			auto longest_line = std::size_t{ 0 };

			while ( start < safe.size( ) ) {
				const auto newline = safe.find( '\n', start );
				const auto end = ( newline == std::string::npos ) ? safe.size( ) : newline;
				const auto length = end - start;

				if ( length > longest_line ) {
					longest_line = length;
				}

				lines.push_back( std::string_view{ safe }.substr( start, length ) );

				if ( newline == std::string::npos ) {
					break;
				}

				start = newline + 1;
			}

			const auto total_lines = lines.size( );

			// An empty file has zero lines and is a legitimate state, not an offset
			// past the end. Without this the default offset of 1 was rejected, so
			// reading a file the agent had just created failed and the read was
			// never recorded -- which then refused the write that followed.
			if ( total_lines > 0 && offset > total_lines ) {
				return error_result( "offset " + std::to_string( offset ) + " is past the end of " +
						std::string{ relative } + " (" + std::to_string( total_lines ) + " lines)",
					"re-read with offset " + std::to_string( total_lines > 0 ? total_lines : 1 ) +
						" or omit offset to start at line 1",
					false );
			}

			const auto first = offset - 1;
			const auto last = std::min( first + limit, total_lines );

			auto rendered = std::string{ };

			for ( auto index = first; index < last; ++index ) {
				rendered += std::to_string( index + 1 );
				rendered += '\t';
				rendered.append( lines[ index ] );
				rendered += '\n';
			}

			auto notes = std::string{ };
			auto truncated_window = last < total_lines;

			if ( truncated_window ) {
				notes += "\"truncated\":true,\"next_offset\":" + std::to_string( last + 1 ) + ",";
			}

			if ( !size_is_complete ) {
				notes += "\"partial_read\":true,";
			}

			if ( longest_line > MINIFIED_LINE_BYTES ) {
				notes += "\"generated\":true,";
			}

			if ( file_size > LARGE_FILE_BYTES || total_lines > 50'000 ) {
				notes += "\"large_file\":true,";
			}

			if ( !notes.empty( ) ) {
				notes.pop_back( );
				rendered += "\n{" + notes + "}";
			}

			// The recorded hash must match what content_hash computes over the RAW
			// bytes, or a non-UTF-8 file reads as stale on the first write. The
			// sanitised text is for display only.
			const auto hash = hash_bytes( content );
			context.reads->record( absolute, hash );

			return rendered;
		}

		[[nodiscard]] auto refuse_binary( const std::string_view relative,
			const std::uintmax_t size, const std::string_view detected ) -> std::string {
			return error_result( std::string{ "refusing to read binary file: " } +
					std::string{ relative },
				"file is " + std::string{ detected } + " (" + std::to_string( size ) +
					" bytes); inspect it with bash, e.g. xxd or strings on the path",
				false );
		}

	}

	auto handle_read( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto path = args.string_field( "path" );

		if ( !path || path->empty( ) ) {
			return error_result( "missing required argument: path",
				"pass the file to read relative to the workspace root, e.g. src/main.cxx", false );
		}

		auto offset = std::size_t{ 1 };
		auto limit = std::size_t{ DEFAULT_READ_LINES };

		if ( const auto requested = args.int_field( "offset" ) ) {
			if ( *requested < 1 ) {
				return error_result( "offset must be 1-based",
					"pass offset >= 1; line numbering starts at 1", false );
			}

			offset = static_cast< std::size_t >( *requested );
		}

		if ( const auto requested = args.int_field( "limit" ) ) {
			if ( *requested < 1 ) {
				return error_result( "limit must be positive",
					"pass limit >= 1; the default window is 100 lines", false );
			}

			limit = static_cast< std::size_t >( std::min< std::int64_t >( *requested, MAX_READ_LINES ) );
		}

		auto& space = *context.space;
		auto resolved = space.resolve( *path );

		auto error_code = std::error_code{ };

		if ( !resolved ||
			!std::filesystem::exists( platform::to_extended_path( *resolved ), error_code ) ||
			error_code ) {
			// Typo recovery: find the closest existing path in the same directory
			// before reporting the failure.
			auto probe = std::filesystem::path{ space.root( ) } / std::filesystem::path{ *path };
			const auto suggestion = closest_existing( space, probe );

			auto hint = std::string{ "check the path; paths are relative to the workspace root" };

			if ( suggestion ) {
				hint = "did you mean " + *suggestion + "?";
			}

			return error_result( "file not found: " + *path, hint, false );
		}

		const auto absolute = *resolved;
		const auto size = std::filesystem::file_size( platform::to_extended_path( absolute ),
			error_code );

		if ( error_code ) {
			return error_result( "cannot stat " + *path + ": " + error_code.message( ),
				"the path resolved but is not a readable file; check it is a file, not a directory",
				false );
		}

		const auto extension = extension_of( absolute );

		if ( binary_extensions::contains( extension ) ) {
			return refuse_binary( *path, size, extension + " file" );
		}

		// The window is served from a bounded streaming read, never the whole-file
		// reader, so a file between the read cap and the write cap is still
		// navigable a window at a time.
		auto content = read_window_bytes( absolute, WINDOW_READ_BYTES );

		if ( !content ) {
			return error_result( content.error( ).msg,
				"check the file is readable; the path is inside the workspace", false );
		}

		const auto complete = content->size( ) < WINDOW_READ_BYTES ||
			static_cast< std::uintmax_t >( content->size( ) ) >= size;

		if ( looks_binary( std::string_view{ *content }.substr( 0, BINARY_PROBE_BYTES ) ) ) {
			return refuse_binary( *path, size, "binary content" );
		}

		return render_window( absolute, *path, offset, limit, *content, size, complete,
			context );
	}

}
