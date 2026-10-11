#include "mcode/support/text.hxx"

#include <simdutf.h>

#include <array>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mcode::text {

	namespace {

		constexpr char32_t REPLACEMENT = 0xFFFD;

		[[nodiscard]] auto encode_utf8( const char32_t codepoint, char* out ) noexcept
			-> std::size_t {
			if ( codepoint > 0x10FFFF || ( codepoint >= 0xD800 && codepoint <= 0xDFFF ) ) {
				return 0;
			}

			if ( codepoint < 0x80 ) {
				out[ 0 ] = static_cast< char >( codepoint );
				return 1;
			}

			if ( codepoint < 0x800 ) {
				out[ 0 ] = static_cast< char >( 0xC0 | ( codepoint >> 6 ) );
				out[ 1 ] = static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
				return 2;
			}

			if ( codepoint < 0x10000 ) {
				out[ 0 ] = static_cast< char >( 0xE0 | ( codepoint >> 12 ) );
				out[ 1 ] = static_cast< char >( 0x80 | ( ( codepoint >> 6 ) & 0x3F ) );
				out[ 2 ] = static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
				return 3;
			}

			out[ 0 ] = static_cast< char >( 0xF0 | ( codepoint >> 18 ) );
			out[ 1 ] = static_cast< char >( 0x80 | ( ( codepoint >> 12 ) & 0x3F ) );
			out[ 2 ] = static_cast< char >( 0x80 | ( ( codepoint >> 6 ) & 0x3F ) );
			out[ 3 ] = static_cast< char >( 0x80 | ( codepoint & 0x3F ) );

			return 4;
		}

		[[nodiscard]] auto sequence_length( const unsigned char byte ) noexcept -> std::size_t {
			if ( byte < 0x80 ) return 1;
			if ( ( byte & 0xE0 ) == 0xC0 ) return 2;
			if ( ( byte & 0xF0 ) == 0xE0 ) return 3;
			if ( ( byte & 0xF8 ) == 0xF0 ) return 4;

			return 0;
		}

	}

	auto is_valid_utf8( const std::string_view input ) noexcept -> bool {
		if ( input.empty( ) ) {
			return true;
		}

		return simdutf::validate_utf8( input.data( ), input.size( ) );
	}

	auto codepoint_count( const std::string_view input ) noexcept -> std::size_t {
		if ( input.empty( ) ) {
			return 0;
		}

		return simdutf::count_utf8( input.data( ), input.size( ) );
	}

	auto truncate_offset( const std::string_view input, const std::size_t max_bytes ) noexcept
		-> std::size_t {
		if ( max_bytes >= input.size( ) ) {
			return input.size( );
		}

		auto offset = max_bytes;
		auto skipped = std::size_t{ 0 };

		while ( offset > 0 && skipped < 4 ) {
			const auto byte = static_cast< unsigned char >( input[ offset ] );

			if ( ( byte & 0xC0 ) != 0x80 ) {
				break;
			}

			--offset;
			++skipped;
		}

		if ( offset == 0 ) {
			return 0;
		}

		const auto length = sequence_length( static_cast< unsigned char >( input[ offset ] ) );

		if ( length == 0 || offset + length > max_bytes ) {
			return offset;
		}

		return offset + length;
	}

	auto truncate( const std::string_view input, const std::size_t max_bytes,
		const std::string_view ellipsis ) -> std::string {
		if ( input.size( ) <= max_bytes ) {
			return std::string{ input };
		}

		const auto cut = truncate_offset( input, max_bytes );
		auto out = std::string{ input.substr( 0, cut ) };
		out.append( ellipsis );

		return out;
	}

	auto sanitize_utf8( const std::string_view input ) -> std::string {
		if ( input.empty( ) ) {
			return { };
		}

		if ( is_valid_utf8( input ) ) {
			return std::string{ input };
		}

		auto out = std::string{ };
		out.reserve( input.size( ) );

		auto encoded = std::array< char, 4 >{ };

		auto index = std::size_t{ 0 };

		while ( index < input.size( ) ) {
			const auto lead = static_cast< unsigned char >( input[ index ] );
			const auto length = sequence_length( lead );

			// The bytes the sequence actually covers: the lead plus the
			// continuation bytes that really followed it, whether the sequence
			// was cut short by a non-continuation or by the end of the input.
			//
			// Advancing by the lead byte's CLAIMED length instead was wrong in
			// both cases: `caf\xe9 x` swallowed the " x" after a lone Latin-1
			// lead, and a sequence truncated at the end of the input
			// (`\xf0\x9f` + "A") advanced past the end and dropped the "A".
			// This feeds the model the file it anchors an edit on, so the loss
			// was silent and the edit then failed to match.
			auto consumed = std::size_t{ 1 };

			if ( length > 1 ) {
				for ( auto continuation = std::size_t{ 1 }; continuation < length;
					++continuation ) {
					if ( index + continuation >= input.size( ) ||
						( static_cast< unsigned char >(
							input[ index + continuation ] ) & 0xC0 ) != 0x80 ) {
						break;
					}

					consumed = continuation + 1;
				}
			}

			auto valid = length > 0 && index + length <= input.size( ) &&
				consumed == length;

			if ( valid ) {
				valid = simdutf::validate_utf8( input.data( ) + index, length );
			}

			if ( valid ) {
				out.append( input.substr( index, length ) );
				index += length;
			} else {
				const auto written = encode_utf8( REPLACEMENT, encoded.data( ) );
				out.append( encoded.data( ), written );
				index += consumed;
			}
		}

		return out;
	}

	auto to_utf16( const std::string_view input ) -> result< std::u16string > {
		if ( input.empty( ) ) {
			return std::u16string{ };
		}

		if ( !is_valid_utf8( input ) ) {
			return std::unexpected( fail( errc::unsupported, "input is not valid UTF-8" ) );
		}

		const auto units = simdutf::utf16_length_from_utf8( input.data( ), input.size( ) );
		auto out = std::u16string( units, u'\0' );
		const auto written = simdutf::convert_utf8_to_utf16le(
			input.data( ), input.size( ), reinterpret_cast< char16_t* >( out.data( ) ) );

		out.resize( written );

		return out;
	}

	auto from_utf16( const std::u16string_view input ) -> result< std::string > {
		if ( input.empty( ) ) {
			return std::string{ };
		}

		const auto bytes = simdutf::utf8_length_from_utf16le( input.data( ), input.size( ) );
		auto out = std::string( bytes, '\0' );
		const auto written = simdutf::convert_utf16le_to_utf8(
			input.data( ), input.size( ), out.data( ) );

		out.resize( written );

		return out;
	}


	auto ascii_lower( const std::string_view input ) -> std::string {
		auto out = std::string{ };
		out.reserve( input.size( ) );

		for ( const auto character : input ) {
			// Arithmetic rather than `std::tolower`: that one is locale-sensitive,
			// so the same byte folds differently in a Turkish locale.
			out.push_back( character >= 'A' && character <= 'Z'
				? static_cast< char >( character - 'A' + 'a' )
				: character );
		}

		return out;
	}

	auto is_ascii_space( const char character ) noexcept -> bool {
		return character == ' ' || character == '\t' || character == '\n'
			|| character == '\r' || character == '\f' || character == '\v';
	}

	auto program_basename( const std::string_view program ) -> std::string {
		// Both separators, because a Windows path reaches here from a config or a
		// command line and may carry either.
		const auto slash = program.find_last_of( "/\\" );
		const auto base = slash == std::string_view::npos
			? program
			: program.substr( slash + 1 );

		return ascii_lower( base );
	}

	// Per-extension memory is a budget the project tracks (`docs/28` measured
	// ~320 KB per VM), and a session's log size is what tells a reader whether it
	// is the one they want. One implementation so the two reports cannot disagree.
	auto human_bytes( const std::uint64_t bytes ) -> std::string {
		constexpr auto BYTES_PER_KILOBYTE = std::uint64_t{ 1024 };
		constexpr auto BYTES_PER_MEGABYTE = BYTES_PER_KILOBYTE * 1024;
		constexpr auto BYTES_PER_GIGABYTE = BYTES_PER_MEGABYTE * 1024;

		// The ladder matters for the values this is used on: a 12 MB session log
		// rendered as "12288 KB" is a number a reader has to convert before they
		// can compare it with the next row.
		if ( bytes >= BYTES_PER_GIGABYTE ) {
			return std::to_string( bytes / BYTES_PER_GIGABYTE ) + " GB";
		}

		if ( bytes >= BYTES_PER_MEGABYTE ) {
			return std::to_string( bytes / BYTES_PER_MEGABYTE ) + " MB";
		}

		if ( bytes >= BYTES_PER_KILOBYTE ) {
			return std::to_string( bytes / BYTES_PER_KILOBYTE ) + " KB";
		}

		return std::to_string( bytes ) + " B";
	}

	namespace {

		// UTF-8 decode of one codepoint; returns the length consumed, 0 on error.
		auto decode_one( const std::string_view input, const std::size_t offset,
			std::uint32_t& out ) -> std::size_t {
			const auto first = static_cast< unsigned char >( input[ offset ] );

			if ( first < 0x80u ) {
				out = first;

				return 1;
			}

			auto length = std::size_t{ 0 };
			auto value = std::uint32_t{ 0 };

			if ( ( first & 0xE0u ) == 0xC0u ) {
				length = 2;
				value = first & 0x1Fu;
			} else if ( ( first & 0xF0u ) == 0xE0u ) {
				length = 3;
				value = first & 0x0Fu;
			} else if ( ( first & 0xF8u ) == 0xF0u ) {
				length = 4;
				value = first & 0x07u;
			} else {
				return 0;
			}

			if ( offset + length > input.size( ) ) {
				return 0;
			}

			for ( auto index = std::size_t{ 1 }; index < length; ++index ) {
				const auto next = static_cast< unsigned char >( input[ offset + index ] );

				if ( ( next & 0xC0u ) != 0x80u ) {
					return 0;
				}

				value = ( value << 6 ) | ( next & 0x3Fu );
			}

			out = value;

			return length;
		}

		auto is_invisible( const std::uint32_t codepoint ) -> bool {
			if ( codepoint == 0x09u || codepoint == 0x0Au || codepoint == 0x0Du ) {
				return false;
			}

			// Cc and Cf: control and format characters, neither of which renders.
			if ( codepoint <= 0x1Fu || ( codepoint >= 0x7Fu && codepoint <= 0x9Fu ) ) {
				return true;
			}

			if ( codepoint == 0xADu || codepoint == 0x061Cu || codepoint == 0x180Eu ) {
				return true;
			}

			if ( codepoint >= 0x200Bu && codepoint <= 0x200Fu ) {
				return true;
			}

			if ( codepoint == 0x2028u || codepoint == 0x2029u || codepoint == 0x202Eu ||
				codepoint == 0x2060u || codepoint == 0xFEFFu ) {
				return true;
			}

			if ( codepoint >= 0x2066u && codepoint <= 0x2069u ) {
				return true;
			}

			// Variation selectors: the GlassWorm payload alphabet.
			if ( codepoint >= 0xFE00u && codepoint <= 0xFE0Fu ) {
				return true;
			}

			if ( codepoint >= 0xE0100u && codepoint <= 0xE01EFu ) {
				return true;
			}

			return false;
		}

	}

	auto count_invisible( const std::string_view input ) noexcept -> std::size_t {
		auto count = std::size_t{ 0 };
		auto offset = std::size_t{ 0 };

		while ( offset < input.size( ) ) {
			auto codepoint = std::uint32_t{ 0 };
			const auto length = decode_one( input, offset, codepoint );

			if ( length == 0 ) {
				++offset;

				continue;
			}

			if ( is_invisible( codepoint ) ) {
				++count;
			}

			offset += length;
		}

		return count;
	}

	auto count_invisible( const std::filesystem::path& directory ) -> std::size_t {
		auto count = std::size_t{ 0 };
		auto error = std::error_code{ };

		if ( !std::filesystem::exists( directory, error ) ) {
			return 0;
		}

		for ( const auto& entry : std::filesystem::recursive_directory_iterator{ directory, error } ) {
			if ( error ) {
				break;
			}

			if ( !entry.is_regular_file( error ) || error ) {
				continue;
			}

			auto stream = std::ifstream{ entry.path( ), std::ios::binary };

			if ( !stream ) {
				continue;
			}

			auto buffer = std::ostringstream{ };
			buffer << stream.rdbuf( );

			count += mcode::text::count_invisible( std::string_view{ buffer.str( ) } );
		}

		return count;
	}

}
