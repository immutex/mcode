#include "mcode/support/text.hxx"

#include <simdutf.h>

#include <array>

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
}
