#include "mcode/tools/file_tools.hxx"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools {

	namespace {

		// refusal is decided on the raw head bytes, before any decoding
		inline constexpr std::size_t BINARY_PROBE_BYTES = 8192;

		// machine-generated output, not something to edit through a viewport
		inline constexpr std::size_t MINIFIED_LINE_BYTES = 5u * 1024u;

		// even a served window gets the large-file stub: navigate deliberately, not blind paging
		inline constexpr std::uintmax_t LARGE_FILE_BYTES = 1u * 1024u * 1024u;

		// keeps a file up to the write cap readable a window at a time
		inline constexpr std::uintmax_t WINDOW_READ_BYTES = 16u * 1024u * 1024u;

		// A single line is unbounded, so one minified file could put its whole
		// window into the context in one call. A line longer than this is cut,
		// and the result says so rather than silently dropping the remainder.
		inline constexpr std::size_t MAX_LINE_BYTES = 8u * 1024u;

		// typo recovery scans the parent directory only, never the tree
		inline constexpr std::size_t SUGGESTION_LIMIT = 12;

		inline constexpr std::size_t SUGGESTION_SCAN_MULTIPLIER = 16;

		inline constexpr std::size_t MAX_READ_LINES = 1000;

		// line-count arm of the large-file note
		inline constexpr std::size_t LARGE_FILE_LINES = 50'000;

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

		// workspace::read_file caps at 8 MiB; the write cap is 10 MiB, so a window read bypasses it
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

			// a larger file is served truncated; the caller marks that window incomplete
			if ( content.size( ) > max_bytes ) {
				content.resize( static_cast< std::size_t >( max_bytes ) );
			}

			return content;
		}

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

				if ( ++candidates > SUGGESTION_LIMIT * SUGGESTION_SCAN_MULTIPLIER ) {
					break;
				}

				const auto name = entry.path( ).filename( ).string( );

				if ( name == wanted ) {
					continue;
				}

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

		struct window_request {
			std::filesystem::path absolute;
			std::string_view relative;
			std::size_t offset = 0;
			std::size_t limit = 0;
			const std::string* content = nullptr;
			std::uintmax_t file_size = 0;
			bool size_is_complete = false;
			tool_context* context = nullptr;
		};

		[[nodiscard]] auto render_window( const window_request& request ) -> result< std::string > {
			const auto safe = text::sanitize_utf8( *request.content );

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

			// an empty file is a legitimate state, not an offset past the end
			if ( total_lines > 0 && request.offset > total_lines ) {
				const auto window_bound = request.size_is_complete
					? std::string{ }
					: std::string{ " (the read window holds the first " } +
						std::to_string( WINDOW_READ_BYTES ) + " bytes of a larger file)";

				return error_result( "offset " + std::to_string( request.offset ) + " is past the end of " +
						std::string{ request.relative } + " (" + std::to_string( total_lines ) +
						" lines)" + window_bound,
					"re-read with offset " + std::to_string( total_lines > 0 ? total_lines : 1 ) +
						" or omit offset to start at line 1",
					false );
			}

			const auto first = request.offset - 1;
			const auto last = std::min( first + request.limit, total_lines );

			auto rendered = std::string{ };
			auto cut_a_line = false;

			for ( auto index = first; index < last; ++index ) {
				rendered += std::to_string( index + 1 );
				rendered += '\t';

				// The window is bounded by LINES, so one minified line put its
				// whole 16 MiB into the context in a single call -- the case this
				// tool already detects and flags as generated. A line is cut at a
				// code-point boundary and says so, rather than being emitted whole.
				const auto& line = lines[ index ];

				if ( line.size( ) > MAX_LINE_BYTES ) {
					rendered.append( line.substr( 0, text::truncate_offset( line,
						MAX_LINE_BYTES ) ) );
					rendered += " …[line truncated at ";
					rendered += std::to_string( MAX_LINE_BYTES );
					rendered += " bytes; re-read with a smaller limit or use grep]";
					cut_a_line = true;
				} else {
					rendered.append( line );
				}

				rendered += '\n';
			}

			auto notes = std::string{ };
			auto truncated_window = last < total_lines;

			// an empty render is indistinguishable from a tool that returned nothing; say so
			if ( total_lines == 0 ) {
				notes += "\"empty\":true,";
			}

			if ( truncated_window ) {
				notes += "\"truncated\":true,\"next_offset\":" + std::to_string( last + 1 ) + ",";
			}

			if ( !request.size_is_complete ) {
				notes += "\"partial_read\":true,";
			}

			if ( longest_line > MINIFIED_LINE_BYTES ) {
				notes += "\"generated\":true,";
			}

			if ( cut_a_line ) {
				notes += "\"line_truncated\":true,";
			}

			if ( request.file_size > LARGE_FILE_BYTES || total_lines > LARGE_FILE_LINES ) {
				notes += "\"large_file\":true,";
			}

			if ( !notes.empty( ) ) {
				notes.pop_back( );
				rendered += "\n{" + notes + "}";
			}

			// hash the RAW bytes: hashing the sanitized text makes a non-UTF-8 file read stale
			const auto hash = hash_bytes( *request.content );
			request.context->reads->record( request.absolute, hash );

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

		if ( !resolved ) {
			// not a refusal: deny rules deny, the default set prompts; resource is canonical
			auto candidate = std::filesystem::path{ space.root( ) } /
				std::filesystem::path{ *path };
			auto canonical = platform::canonicalize( candidate );

			if ( !canonical ) {
				return error_result( "cannot resolve " + *path + ": " + resolved.error( ).msg,
					"check the path; paths are relative to the workspace root", false );
			}

			auto request = perm::permission_request{ };
			request.tool_name = "read";
			request.klass = tool_class::read;
			request.resource = canonical->generic_string( );

			if ( context.permissions->decide( request ) != perm::permission_decision::allow ) {
				const auto& verdict = context.permissions->last_verdict( );

				return error_result(
					"read denied by the permission engine: " + *path +
						( verdict.reason.empty( ) ? std::string{ } : " (" + verdict.reason + ")" ),
					"the path is outside the workspace; add an allow rule for it or run interactively to approve",
					false );
			}

			resolved = *canonical;
		}

		auto error_code = std::error_code{ };

		if ( !resolved ||
			!std::filesystem::exists( platform::to_extended_path( *resolved ), error_code ) ||
			error_code ) {
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

		auto window = window_request{ };
		window.absolute = absolute;
		window.relative = *path;
		window.offset = offset;
		window.limit = limit;
		window.content = &*content;
		window.file_size = size;
		window.size_is_complete = complete;
		window.context = &context;

		return render_window( window );
	}

}
