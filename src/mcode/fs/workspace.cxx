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

		// A judgement, not a derivation: this share of non-printable probe bytes marks binary.
		constexpr std::size_t SUSPICIOUS_PERCENT = 10;
		constexpr std::size_t PERCENT_SCALE = 100;

		// Bounds the recursion so a pathological tree is a slow listing, not a stack overflow.
		constexpr std::size_t MAX_GLOB_DEPTH = 64;

		constexpr std::uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
		constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

		constexpr std::size_t HASH_HEX_BUFFER = 17;

		[[nodiscard]] auto segment_matches( const std::string_view pattern,
			const std::string_view name ) -> bool {
			auto pattern_index = std::size_t{ 0 };
			auto name_index = std::size_t{ 0 };
			auto star_pattern = std::string_view::npos;
			auto star_name = std::size_t{ 0 };

			while ( name_index < name.size( ) ) {
				if ( pattern_index < pattern.size( ) &&
					( pattern[ pattern_index ] == '?' ||
						pattern[ pattern_index ] == name[ name_index ] ) ) {
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

		[[nodiscard]] auto split_pattern( const std::string_view pattern )
			-> std::vector< std::string > {
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

		struct glob_request {
			const std::vector< std::string >& parts;
			std::vector< std::filesystem::path >& out;
			std::size_t max_results = 0;

			// Guards the `**` branch against a symlink loop, which the match cap cannot.
			std::set< std::filesystem::path >& visited;

			std::size_t depth = 0;
		};

		// symlink_status does not follow the link, so a symlinked directory is never descended.
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

				for ( const auto& entry : std::filesystem::directory_iterator(
					platform::to_extended_path( base ),
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

			for ( const auto& entry : std::filesystem::directory_iterator(
				platform::to_extended_path( base ),
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
		auto hash = FNV_OFFSET_BASIS;

		for ( const auto byte : bytes ) {
			hash ^= static_cast< unsigned char >( byte );
			hash *= FNV_PRIME;
		}

		auto buffer = std::array< char, HASH_HEX_BUFFER >{ };
		std::snprintf( buffer.data( ), buffer.size( ), "%016llx",
			static_cast< unsigned long long >( hash ) );

		return std::string{ buffer.data( ) };
	}

	auto workspace::open( const std::filesystem::path& root ) -> result< workspace > {
		auto error_code = std::error_code{ };

		if ( !std::filesystem::exists( root, error_code ) || error_code ) {
			return std::unexpected(
				fail( errc::io, "workspace root does not exist: " + root.string( ) ) );
		}

		if ( !std::filesystem::is_directory( root, error_code ) || error_code ) {
			return std::unexpected(
				fail( errc::io, "workspace root is not a directory: " + root.string( ) ) );
		}

		auto opened = workspace{ };
		opened.canonical_root_ = std::filesystem::weakly_canonical( root, error_code );

		if ( error_code ) {
			return std::unexpected(
				fail( errc::io, "cannot canonicalize workspace root: " + error_code.message( ) ) );
		}

		return opened;
	}

	auto workspace::resolve( const std::string_view path ) const
		-> result< std::filesystem::path > {
		if ( path.empty( ) ) {
			return std::unexpected( fail( errc::io, "empty path" ) );
		}

		auto candidate = std::filesystem::path{ std::string{ path } };

		if ( candidate.is_relative( ) ) {
			candidate = canonical_root_ / candidate;
		}

		// Through the seam: it owns the long-path form, and the stored path drops the prefix.
		auto canonical = platform::canonicalize( candidate );

		if ( !canonical ) {
			return std::unexpected(
				fail( errc::io, "cannot resolve path: " + canonical.error( ).msg ) );
		}

		if ( !contains( *canonical ) ) {
			return std::unexpected( fail( errc::io,
				"path escapes the workspace: " + canonical->string( ) ) );
		}

		return *canonical;
	}

	auto workspace::contains( const std::filesystem::path& absolute ) const -> bool {
		// weakly_canonical so a short-name or not-yet-existing path still compares correctly.
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

	auto workspace::read_file( const std::string_view relative_path ) const
		-> result< std::string > {
		auto resolved = resolve( relative_path );

		if ( !resolved ) {
			return std::unexpected( resolved.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto size = std::filesystem::file_size( platform::to_extended_path( *resolved ),
			error_code );

		if ( error_code ) {
			return std::unexpected(
				fail( errc::io,
					"cannot stat " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		if ( size > MAX_TEXT_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "file exceeds the " +
				std::to_string( MAX_TEXT_FILE_BYTES ) + "-byte read cap: " +
					resolved->string( ) ) );
		}

		// The \\?\ prefix only at the syscall: without it a deep path fails on a stock Windows.
		auto input = std::ifstream{ platform::to_extended_path( *resolved ), std::ios::binary };

		if ( !input ) {
			return std::unexpected( fail( errc::io, "cannot open " + resolved->string( ) ) );
		}

		// Read one byte past the cap: the stat is only a fast reject, and the file can grow.
		auto content = std::string{ };
		content.resize( MAX_TEXT_FILE_BYTES + 1 );

		input.read( content.data( ), static_cast< std::streamsize >( content.size( ) ) );
		content.resize( static_cast< std::size_t >( input.gcount( ) ) );

		if ( content.size( ) > MAX_TEXT_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "file exceeds the " +
				std::to_string( MAX_TEXT_FILE_BYTES ) + "-byte read cap: " +
					resolved->string( ) ) );
		}

		if ( looks_binary( content ) ) {
			return std::unexpected(
				fail( errc::io, "refusing to read binary file: " + resolved->string( ) ) );
		}

		return content;
	}

	auto workspace::content_hash( const std::string_view relative_path ) const
		-> result< std::string > {
		auto content = read_file( relative_path );

		if ( !content ) {
			return std::unexpected( content.error( ) );
		}

		return hash_bytes( *content );
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
