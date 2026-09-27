#include "mcode/fs/workspace.hxx"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "mcode/support/text.hxx"

namespace mcode {

	namespace {

		constexpr std::size_t BINARY_PROBE_BYTES = 8192;
		constexpr std::size_t MAX_CONTINUATION_BYTES = 4;

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

		auto glob_walk( const std::filesystem::path& base, const std::vector< std::string >& parts,
			const std::size_t index, std::vector< std::filesystem::path >& out,
			const std::size_t max_results ) -> void {
			if ( out.size( ) >= max_results ) {
				return;
			}

			if ( index >= parts.size( ) ) {
				out.push_back( base );
				return;
			}

			const auto& part = parts[ index ];
			const auto last = ( index + 1 == parts.size( ) );

			if ( part == "**" ) {
				glob_walk( base, parts, index + 1, out, max_results );

				auto error_code = std::error_code{ };

				for ( const auto& entry : std::filesystem::directory_iterator( base,
						 std::filesystem::directory_options::skip_permission_denied, error_code ) ) {
					if ( error_code ) {
						break;
					}

					if ( entry.is_directory( error_code ) && !error_code ) {
						glob_walk( entry.path( ), parts, index, out, max_results );
					}
				}

				return;
			}

			auto error_code = std::error_code{ };

			for ( const auto& entry : std::filesystem::directory_iterator( base,
					 std::filesystem::directory_options::skip_permission_denied, error_code ) ) {
				if ( error_code ) {
					break;
				}

				const auto name = entry.path( ).filename( ).string( );

				if ( !segment_matches( part, name ) ) {
					continue;
				}

				if ( last ) {
					out.push_back( entry.path( ) );

					if ( out.size( ) >= max_results ) {
						return;
					}
				} else if ( entry.is_directory( error_code ) && !error_code ) {
					glob_walk( entry.path( ), parts, index + 1, out, max_results );
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

		return probe > 0 && ( suspicious * 100 / probe ) > 10;
	}

	auto hash_bytes( const std::string_view bytes ) noexcept -> std::string {
		auto hash = std::uint64_t{ 1469598103934665603ULL };

		for ( const auto byte : bytes ) {
			hash ^= static_cast< unsigned char >( byte );
			hash *= 1099511628211ULL;
		}

		auto buffer = std::array< char, 17 >{ };
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

		auto error_code = std::error_code{ };
		auto candidate = std::filesystem::path{ std::string{ path } };

		if ( candidate.is_relative( ) ) {
			candidate = canonical_root_ / candidate;
		}

		const auto canonical = std::filesystem::weakly_canonical( candidate, error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io, "cannot resolve path: " + error_code.message( ) ) );
		}

		if ( !contains( canonical ) ) {
			return std::unexpected( fail( errc::io, "path escapes the workspace: " + canonical.string( ) ) );
		}

		return canonical;
	}

	auto workspace::contains( const std::filesystem::path& absolute ) const -> bool {
		auto root_entry = canonical_root_.begin( );
		auto path_entry = absolute.begin( );

		for ( ; root_entry != canonical_root_.end( ); ++root_entry, ++path_entry ) {
			if ( path_entry == absolute.end( ) ) {
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
		glob_walk( canonical_root_, parts, 0, matches, max_results );

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
		const auto size = std::filesystem::file_size( *resolved, error_code );

		if ( error_code ) {
			return std::unexpected(
				fail( errc::io, "cannot stat " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		if ( size > MAX_TEXT_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "file exceeds the " +
				std::to_string( MAX_TEXT_FILE_BYTES ) + "-byte read cap: " + resolved->string( ) ) );
		}

		auto input = std::ifstream{ *resolved, std::ios::binary };

		if ( !input ) {
			return std::unexpected( fail( errc::io, "cannot open " + resolved->string( ) ) );
		}

		auto stream = std::ostringstream{ };
		stream << input.rdbuf( );
		auto content = stream.str( );

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
