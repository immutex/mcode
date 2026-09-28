#include "mcode/fs/workspace.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "mcode/platform/seams.hxx"
#include "mcode/support/text.hxx"
#include "mcode/support/time.hxx"

namespace mcode {

	namespace {

		constexpr std::size_t BINARY_PROBE_BYTES = 8192;

		// A file is "binary" when this share of its probed bytes are non-printable
		// controls. Named because the threshold is a judgement, not a derivation.
		constexpr std::size_t SUSPICIOUS_PERCENT = 10;
		constexpr std::size_t PERCENT_SCALE = 100;

		// A `**` walk this deep is a pathological tree, and the bound is what keeps
		// the recursion from being a stack overflow rather than a slow listing.
		constexpr std::size_t MAX_GLOB_DEPTH = 64;

		// FNV-1a, 64-bit.
		constexpr std::uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
		constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

		// Sixteen hex digits plus the terminator.
		constexpr std::size_t HASH_HEX_BUFFER = 17;

		[[nodiscard]] auto segment_matches( const std::string_view pattern, const std::string_view name ) -> bool {
			auto pattern_index = std::size_t{ 0 };
			auto name_index = std::size_t{ 0 };
			auto star_pattern = std::string_view::npos;
			auto star_name = std::size_t{ 0 };

			while ( name_index < name.size( ) ) {
				if ( pattern_index < pattern.size( ) &&
					( pattern[ pattern_index ] == '?' || pattern[ pattern_index ] == name[ name_index ] ) ) {
					++pattern_index;
					++name_index;
				} else if ( pattern_index < pattern.size( ) && pattern[ pattern_index ] == '*' ) {
					star_pattern = pattern_index++;
					star_name = name_index;
				} else if ( star_pattern != std::string_view::npos ) {
					pattern_index = star_pattern + 1;
					name_index = ++star_name;
				} else {
					return false;
				}
			}

			while ( pattern_index < pattern.size( ) && pattern[ pattern_index ] == '*' ) {
				++pattern_index;
			}

			return pattern_index == pattern.size( );
		}

		[[nodiscard]] auto split_pattern( const std::string_view pattern ) -> std::vector< std::string > {
			auto parts = std::vector< std::string >{ };
			auto start = std::size_t{ 0 };

			while ( start <= pattern.size( ) ) {
				const auto slash = pattern.find( '/', start );
				const auto end = ( slash == std::string_view::npos ) ? pattern.size( ) : slash;

				if ( end > start ) {
					parts.emplace_back( pattern.substr( start, end - start ) );
				}

				if ( slash == std::string_view::npos ) {
					break;
				}

				start = slash + 1;
			}

			return parts;
		}

		// The walk's accumulators, so the recursion takes a request rather than five
		// positional parameters.
		struct glob_request {
			const std::vector< std::string >& parts;
			std::vector< std::filesystem::path >& out;
			std::size_t max_results = 0;

			// Directories already descended into. A symlink loop (a -> b -> a) makes
			// the `**` branch recurse forever, and the result cap does not stop it
			// because the cap counts matches, not visits. Keyed by the weakly
			// canonical path so two routes to one directory count as one.
			std::set< std::filesystem::path >& visited;

			// A `**` walk is a filesystem traversal, and an unbounded one on a deep
			// tree is a denial of service even without a cycle.
			std::size_t depth = 0;
		};

		// Enters a directory if it is a real directory and has not been visited.
		// symlink_status does not follow the link, so a symlinked directory is never
		// descended into -- which is both the loop guard and what keeps a listing
		// inside the workspace.
		auto enter_directory( const std::filesystem::path& path, glob_request& request )
			-> bool {
			auto error_code = std::error_code{ };
			const auto status = std::filesystem::symlink_status( path, error_code );

			if ( error_code || !std::filesystem::is_directory( status ) ) {
				return false;
			}

			if ( request.depth >= MAX_GLOB_DEPTH ) {
				return false;
			}

			auto canonical = std::filesystem::weakly_canonical( path, error_code );

			if ( error_code ) {
				return false;
			}

			return request.visited.insert( canonical ).second;
		}

		auto glob_walk( const std::filesystem::path& base, const std::size_t index,
			glob_request& request ) -> void {
			if ( request.out.size( ) >= request.max_results ) {
				return;
			}

			if ( index >= request.parts.size( ) ) {
				request.out.push_back( base );

				return;
			}

			const auto& part = request.parts[ index ];
			const auto last = ( index + 1 == request.parts.size( ) );

			if ( part == "**" ) {
				glob_walk( base, index + 1, request );

				auto error_code = std::error_code{ };

				for ( const auto& entry : std::filesystem::directory_iterator( platform::to_extended_path( base ),
						 std::filesystem::directory_options::skip_permission_denied, error_code ) ) {
					if ( error_code ) {
						break;
					}

					if ( request.out.size( ) >= request.max_results ) {
						return;
					}

					if ( !enter_directory( entry.path( ), request ) ) {
						continue;
					}

					++request.depth;
					glob_walk( entry.path( ), index, request );
					--request.depth;
				}

				return;
			}

			auto error_code = std::error_code{ };

			for ( const auto& entry : std::filesystem::directory_iterator( platform::to_extended_path( base ),
					 std::filesystem::directory_options::skip_permission_denied, error_code ) ) {
				if ( error_code ) {
					break;
				}

				const auto name = entry.path( ).filename( ).string( );

				if ( !segment_matches( part, name ) ) {
					continue;
				}

				if ( last ) {
					request.out.push_back( entry.path( ) );

					if ( request.out.size( ) >= request.max_results ) {
						return;
					}
				} else if ( enter_directory( entry.path( ), request ) ) {
					++request.depth;
					glob_walk( entry.path( ), index + 1, request );
					--request.depth;
				}
			}
		}

	}

	auto looks_binary( const std::string_view bytes ) noexcept -> bool {
		const auto probe = std::min( bytes.size( ), BINARY_PROBE_BYTES );
		auto suspicious = std::size_t{ 0 };

		for ( auto index = std::size_t{ 0 }; index < probe; ++index ) {
			const auto byte = static_cast< unsigned char >( bytes[ index ] );

			if ( byte == 0x00 ) {
				return true;
			}

			if ( byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f' ) {
				++suspicious;
			}
		}

		return probe > 0 && ( suspicious * PERCENT_SCALE / probe ) > SUSPICIOUS_PERCENT;
	}

	auto hash_bytes( const std::string_view bytes ) noexcept -> std::string {
		// FNV-1a, 64-bit. Named because the two constants are the algorithm, and a
		// transcription error in either is silent.
		auto hash = FNV_OFFSET_BASIS;

		for ( const auto byte : bytes ) {
			hash ^= static_cast< unsigned char >( byte );
			hash *= FNV_PRIME;
		}

		auto buffer = std::array< char, HASH_HEX_BUFFER >{ };
		std::snprintf( buffer.data( ), buffer.size( ), "%016llx", static_cast< unsigned long long >( hash ) );

		return std::string{ buffer.data( ) };
	}

	auto workspace::open( const std::filesystem::path& root ) -> result< workspace > {
		auto error_code = std::error_code{ };

		if ( !std::filesystem::exists( root, error_code ) || error_code ) {
			return std::unexpected( fail( errc::io, "workspace root does not exist: " + root.string( ) ) );
		}

		if ( !std::filesystem::is_directory( root, error_code ) || error_code ) {
			return std::unexpected( fail( errc::io, "workspace root is not a directory: " + root.string( ) ) );
		}

		auto opened = workspace{ };
		opened.canonical_root_ = std::filesystem::weakly_canonical( root, error_code );

		if ( error_code ) {
			return std::unexpected(
				fail( errc::io, "cannot canonicalize workspace root: " + error_code.message( ) ) );
		}

		return opened;
	}

	auto workspace::resolve( const std::string_view path ) const -> result< std::filesystem::path > {
		if ( path.empty( ) ) {
			return std::unexpected( fail( errc::io, "empty path" ) );
		}

		auto candidate = std::filesystem::path{ std::string{ path } };

		if ( candidate.is_relative( ) ) {
			candidate = canonical_root_ / candidate;
		}

		// Through the platform seam, not std::filesystem directly: the seam owns the
		// long-path form, and a second canonicalization here is the duplication that
		// drifts. The result is stored WITHOUT the extended prefix, so the paths the
		// model and the logs see stay readable.
		auto canonical = platform::canonicalize( candidate );

		if ( !canonical ) {
			return std::unexpected( fail( errc::io, "cannot resolve path: " + canonical.error( ).msg ) );
		}

		if ( !contains( *canonical ) ) {
			return std::unexpected( fail( errc::io,
				"path escapes the workspace: " + canonical->string( ) ) );
		}

		return *canonical;
	}

	auto workspace::contains( const std::filesystem::path& absolute ) const -> bool {
		// The root is canonical, so the input must be too or the comparison is
		// between two spellings of the same directory. Windows makes this concrete:
		// `%TEMP%` commonly arrives as an 8.3 short name (`RUNNER~1`), which is the
		// same directory as its long form and compares unequal component-wise.
		// weakly_canonical resolves the existing prefix and leaves the rest lexical,
		// so a path that does not exist yet still compares correctly -- and a
		// sibling that merely shares a string prefix still fails, which is the
		// guarantee this function exists to provide.
		auto error_code = std::error_code{ };
		const auto canonical = std::filesystem::weakly_canonical( absolute, error_code );

		if ( error_code ) {
			return false;
		}

		auto root_entry = canonical_root_.begin( );
		auto path_entry = canonical.begin( );

		for ( ; root_entry != canonical_root_.end( ); ++root_entry, ++path_entry ) {
			if ( path_entry == canonical.end( ) ) {
				return false;
			}

			if ( *root_entry != *path_entry ) {
				return false;
			}
		}

		return true;
	}

	auto workspace::glob( const std::string_view pattern, const std::size_t max_results ) const
		-> result< std::vector< std::filesystem::path > > {
		const auto parts = split_pattern( pattern );

		if ( parts.empty( ) ) {
			return std::unexpected( fail( errc::io, "empty glob pattern" ) );
		}

		auto matches = std::vector< std::filesystem::path >{ };
		auto visited = std::set< std::filesystem::path >{ };
		auto request = glob_request{ .parts = parts, .out = matches, .max_results = max_results,
			.visited = visited };

		glob_walk( canonical_root_, 0, request );

		std::sort( matches.begin( ), matches.end( ) );
		matches.erase( std::unique( matches.begin( ), matches.end( ) ), matches.end( ) );

		if ( matches.size( ) > max_results ) {
			matches.resize( max_results );
		}

		return matches;
	}

	auto workspace::read_file( const std::string_view relative_path ) const -> result< std::string > {
		auto resolved = resolve( relative_path );

		if ( !resolved ) {
			return std::unexpected( resolved.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto size = std::filesystem::file_size( platform::to_extended_path( *resolved ),
			error_code );

		if ( error_code ) {
			return std::unexpected(
				fail( errc::io, "cannot stat " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		if ( size > MAX_TEXT_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "file exceeds the " +
				std::to_string( MAX_TEXT_FILE_BYTES ) + "-byte read cap: " + resolved->string( ) ) );
		}

		// The extended form at the syscall boundary only. Windows caps a path at
		// MAX_PATH unless it carries the \\?\ prefix or the machine sets
		// LongPathsEnabled -- and that registry value defaults to 0, so a deep cloned
		// repository fails to open on a stock machine. The prefix is applied here and
		// not to `resolved`, so the path the caller sees stays the readable one.
		auto input = std::ifstream{ platform::to_extended_path( *resolved ), std::ios::binary };

		if ( !input ) {
			return std::unexpected( fail( errc::io, "cannot open " + resolved->string( ) ) );
		}

		// Read one byte past the cap, not the whole file. The stat above is only a
		// fast reject: a file that grows between the stat and the read -- ordinary in
		// a workspace an agent is editing -- would otherwise be slurped in full, and
		// the cap is the only bound on this path.
		auto content = std::string{ };
		content.resize( MAX_TEXT_FILE_BYTES + 1 );

		input.read( content.data( ), static_cast< std::streamsize >( content.size( ) ) );
		content.resize( static_cast< std::size_t >( input.gcount( ) ) );

		if ( content.size( ) > MAX_TEXT_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "file exceeds the " +
				std::to_string( MAX_TEXT_FILE_BYTES ) + "-byte read cap: " + resolved->string( ) ) );
		}

		if ( looks_binary( content ) ) {
			return std::unexpected(
				fail( errc::io, "refusing to read binary file: " + resolved->string( ) ) );
		}

		return content;
	}

	auto workspace::content_hash( const std::string_view relative_path ) const -> result< std::string > {
		auto content = read_file( relative_path );

		if ( !content ) {
			return std::unexpected( content.error( ) );
		}

		return hash_bytes( *content );
	}

	auto workspace::is_protected( const std::filesystem::path& absolute ) const -> bool {
		auto error_code = std::error_code{ };
		const auto canonical = std::filesystem::weakly_canonical( absolute, error_code );

		if ( error_code ) {
			return false;
		}

		auto root_entry = canonical_root_.begin( );
		auto path_entry = canonical.begin( );

		for ( ; root_entry != canonical_root_.end( ); ++root_entry, ++path_entry ) {
			if ( path_entry == canonical.end( ) ) {
				return false;
			}

			if ( *root_entry != *path_entry ) {
				return false;
			}
		}

		// The first component BELOW the root, which is what the rule names. A file
		// called `notes.mcode` at the root is not protected; `.mcode/` is.
		if ( path_entry == canonical.end( ) ) {
			return false;
		}

		const auto first = path_entry->string( );

		return first == ".mcode" || first == ".git";
	}

	auto workspace::write_file( const std::string_view relative_path, const std::string_view content,
		const write_mode mode ) -> result< write_receipt > {
		if ( content.size( ) > MAX_WRITE_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "content exceeds the " +
				std::to_string( MAX_WRITE_FILE_BYTES ) + "-byte write cap" ) );
		}

		auto resolved = resolve( relative_path );

		if ( !resolved ) {
			return std::unexpected( resolved.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( *resolved ), error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot stat " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		if ( mode == write_mode::create && exists ) {
			return std::unexpected( fail( errc::io,
				"file already exists: " + resolved->string( ) ) );
		}

		if ( mode == write_mode::overwrite && !exists ) {
			return std::unexpected( fail( errc::io,
				"file does not exist: " + resolved->string( ) ) );
		}

		if ( mode == write_mode::create ) {
			const auto parent = resolved->parent_path( );

			if ( !parent.empty( ) ) {
				std::filesystem::create_directories( platform::to_extended_path( parent ), error_code );

				if ( error_code ) {
					return std::unexpected( fail( errc::io, "cannot create " + parent.string( ) +
						": " + error_code.message( ) ) );
				}
			}
		}

		// A sibling temp file, so the rename is same-filesystem and therefore
		// atomic. The counter keeps two writes in one process apart; the timestamp
		// keeps two processes apart. Concurrent writers to the SAME file from
		// different processes are not supported -- the harness serializes tool
		// calls -- and this is what makes that limit visible rather than silent.
		static auto counter = std::atomic< std::uint64_t >{ 0 };

		const auto stamp = std::to_string( support::epoch_milliseconds( ) ) + "-" +
			std::to_string( counter.fetch_add( 1, std::memory_order_relaxed ) );

		auto temporary = *resolved;
		temporary += ".mcode-tmp-" + stamp;

		{
			auto output = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !output ) {
				return std::unexpected( fail( errc::io, "cannot open " + temporary.string( ) ) );
			}

			output.write( content.data( ), static_cast< std::streamsize >( content.size( ) ) );

			if ( !output ) {
				output.close( );
				std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

				return std::unexpected( fail( errc::io,
					"failed to write " + temporary.string( ) ) );
			}

			output.close( );

			if ( !output ) {
				std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

				return std::unexpected( fail( errc::io,
					"failed to flush " + temporary.string( ) ) );
			}
		}

		// Replaces on all three platforms: POSIX `rename`, and MSVC routes
		// std::filesystem::rename through MoveFileExW with MOVEFILE_REPLACE_EXISTING.
		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( *resolved ), error_code );

		if ( error_code ) {
			std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

			return std::unexpected( fail( errc::io,
				"cannot replace " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		auto receipt = write_receipt{ };
		receipt.bytes_written = content.size( );
		receipt.content_hash = hash_bytes( content );

		return receipt;
	}

	auto workspace::read_viewport( const std::string_view relative_path, const std::size_t offset,
		const std::size_t limit ) const -> result< read_result > {
		if ( offset == 0 ) {
			return std::unexpected( fail( errc::io, "line offset is 1-based" ) );
		}

		if ( limit == 0 ) {
			return std::unexpected( fail( errc::io, "line limit must be positive" ) );
		}

		auto content = read_file( relative_path );

		if ( !content ) {
			return std::unexpected( content.error( ) );
		}

		const auto safe = text::sanitize_utf8( *content );

		auto lines = std::vector< std::string_view >{ };
		auto start = std::size_t{ 0 };

		while ( start < safe.size( ) ) {
			const auto newline = safe.find( '\n', start );
			const auto end = ( newline == std::string::npos ) ? safe.size( ) : newline;

			lines.push_back( std::string_view{ safe }.substr( start, end - start ) );

			if ( newline == std::string::npos ) {
				break;
			}

			start = newline + 1;
		}

		auto out = read_result{ };
		out.total_lines = lines.size( );

		if ( offset > lines.size( ) ) {
			out.first_line = offset;
			out.last_line = offset - 1;

			return out;
		}

		const auto first = offset - 1;
		const auto last = std::min( first + limit, lines.size( ) );

		auto rendered = std::string{ };

		for ( auto index = first; index < last; ++index ) {
			rendered += std::to_string( index + 1 );
			rendered += '\t';
			rendered.append( lines[ index ] );
			rendered += '\n';
		}

		out.text = std::move( rendered );
		out.first_line = offset;
		out.last_line = last;
		out.truncated = last < lines.size( );

		return out;
	}

	auto workspace::display_path( const std::filesystem::path& path ) const -> std::string {
		auto error_code = std::error_code{ };
		const auto relative = std::filesystem::relative( path, canonical_root_, error_code );
		auto text = ( error_code || relative.empty( ) ) ? path.string( ) : relative.string( );

		std::replace( text.begin( ), text.end( ), '\\', '/' );

		return text;
	}

}
