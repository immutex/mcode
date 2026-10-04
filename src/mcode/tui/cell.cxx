#include "mcode/tui/cell.hxx"

#include <algorithm>
#include <array>
#include <utility>

namespace mcode::tui {

	namespace {

		// East-Asian Wide and Fullwidth ranges. The table is the Unicode 15
		// EAW=W/F set, compressed to the ranges that matter for a terminal;
		// pinned to Unicode 15 so every component agrees.
		constexpr auto WIDE_RANGES = std::array< std::pair< char32_t, char32_t >, 48 >{ {
			{ 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A },
			{ 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
			{ 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 },
			{ 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
			{ 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
			{ 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
			{ 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA },
			{ 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
			{ 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
			{ 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
			{ 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C },
			{ 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x2E80, 0x303E },
			{ 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
			{ 0xA000, 0xA4CF }, { 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 },
			{ 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F },
			{ 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 },
		} };

		// Ambiguous-width code points, the subset a transcript plausibly
		// carries: box-drawing look-alikes, punctuation, Greek and Cyrillic
		// letters. The policy is per session, not per cell.
		constexpr auto AMBIGUOUS_RANGES = std::array< std::pair< char32_t, char32_t >, 30 >{ {
			{ 0x00A1, 0x00A1 }, { 0x00A4, 0x00A4 }, { 0x00A7, 0x00A8 },
			{ 0x00B0, 0x00B1 }, { 0x00B6, 0x00B6 }, { 0x00BB, 0x00BB },
			{ 0x00BF, 0x00BF }, { 0x0391, 0x03A9 }, { 0x03B1, 0x03C9 },
			{ 0x0401, 0x0401 }, { 0x0410, 0x044F }, { 0x0451, 0x0451 },
			{ 0x2010, 0x2010 }, { 0x2018, 0x2019 }, { 0x201C, 0x201D },
			{ 0x2020, 0x2022 }, { 0x2026, 0x2026 }, { 0x2032, 0x2033 },
			{ 0x203B, 0x203B }, { 0x207F, 0x207F }, { 0x20A9, 0x20A9 },
			{ 0x2103, 0x2103 }, { 0x2126, 0x2126 }, { 0x2190, 0x2199 },
			{ 0x21B8, 0x21B9 }, { 0x2212, 0x2212 }, { 0x221A, 0x221A },
			{ 0x2261, 0x2261 }, { 0x2266, 0x2267 }, { 0x2500, 0x254B },
		} };

		template< std::size_t RangeCount >
		[[nodiscard]] auto covers( const char32_t value,
			const std::array< std::pair< char32_t, char32_t >, RangeCount >& table ) noexcept -> bool {
			auto low = std::size_t{ 0 };
			auto high = table.size( );

			while ( low < high ) {
				const auto middle = low + ( high - low ) / 2;

				if ( value < table[ middle ].first ) {
					high = middle;
				} else if ( value > table[ middle ].second ) {
					low = middle + 1;
				} else {
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] auto is_wide_codepoint( const char32_t value ) noexcept -> bool {
			return covers( value, WIDE_RANGES );
		}

		[[nodiscard]] auto is_ambiguous_codepoint( const char32_t value ) noexcept -> bool {
			return covers( value, AMBIGUOUS_RANGES );
		}

		[[nodiscard]] auto is_emoji_presentation( const char32_t value ) noexcept -> bool {
			// The Emoji_Presentation property, the ranges that force width 2
			// even without VS16.
			return ( value >= 0x1F300 && value <= 0x1F64F ) ||
				( value >= 0x1F680 && value <= 0x1F6FF ) ||
				( value >= 0x1F900 && value <= 0x1F9FF ) ||
				( value >= 0x1FA70 && value <= 0x1FAFF ) ||
				( value >= 0x1F000 && value <= 0x1F02F ) ||
				( value >= 0x1F0A0 && value <= 0x1F0FF ) ||
				value == 0x231A || value == 0x231B || value == 0x26A1 ||
				value == 0x2B1B || value == 0x2B50 || value == 0x1F32D ||
				value == 0x1F44D || value == 0x1F44E;
		}

		[[nodiscard]] auto is_regional_indicator( const char32_t value ) noexcept -> bool {
			return value >= 0x1F1E6 && value <= 0x1F1FF;
		}

		[[nodiscard]] auto is_skin_tone_modifier( const char32_t value ) noexcept -> bool {
			return value >= 0x1F3FB && value <= 0x1F3FF;
		}

		[[nodiscard]] auto is_keycap_mark( const char32_t value ) noexcept -> bool {
			return value == 0x20E3;
		}

	}

	auto decode_utf8( const std::string_view text ) noexcept -> utf8_decoded {
		if ( text.empty( ) ) {
			return { };
		}

		const auto lead = static_cast< unsigned char >( text.front( ) );

		if ( lead < 0x80 ) {
			return { lead, 1 };
		}

		const auto expected = utf8_lead_length( text.front( ) );

		if ( expected == 0 ) {
			return { 0xFFFD, 1 };
		}

		if ( text.size( ) < expected ) {
			return { 0xFFFD, 1 };
		}

		char32_t value = 0;

		if ( expected == 2 ) {
			value = lead & 0x1F;
		} else if ( expected == 3 ) {
			value = lead & 0x0F;
		} else {
			value = lead & 0x07;
		}

		for ( auto index = std::size_t{ 1 }; index < expected; ++index ) {
			const auto byte = static_cast< unsigned char >( text[ index ] );

			if ( ( byte & 0xC0 ) != 0x80 ) {
				return { 0xFFFD, 1 };
			}

			value = ( value << 6 ) | ( byte & 0x3F );
		}

		// Overlong encodings and surrogates are invalid UTF-8.
		if ( ( expected == 2 && value < 0x80 ) || ( expected == 3 && value < 0x800 ) ||
			( expected == 4 && value < 0x10000 ) ||
			( value >= 0xD800 && value <= 0xDFFF ) || value > 0x10FFFF ) {
			return { 0xFFFD, 1 };
		}

		return { value, expected };
	}

	auto utf8_lead_length( const char lead ) noexcept -> std::size_t {
		const auto value = static_cast< unsigned char >( lead );

		if ( value < 0x80 ) {
			return 1;
		}

		// excludes the overlong leads 0xC0/0xC1 and the out-of-range 0xF5-0xFF
		if ( value >= 0xC2 && value <= 0xDF ) {
			return 2;
		}

		if ( value >= 0xE0 && value <= 0xEF ) {
			return 3;
		}

		if ( value >= 0xF0 && value <= 0xF4 ) {
			return 4;
		}

		return 0;
	}

	auto span_style( const styled_span& value ) noexcept -> style {
		auto out = style{ };
		out.foreground = value.color;
		out.background = value.background;
		out.bold = value.bold;
		out.italic = value.italic;
		out.dim = value.dim;

		return out;
	}

	auto codepoint_width( const char32_t value, const std::size_t ambiguous_width ) noexcept
		-> std::size_t {
		if ( value == 0 ) {
			return 0;
		}

		// Combining marks and zero-width joiners take no column. The mark
		// ranges are General_Category = Mn/Mc/Me and the common diacritics.
		if ( value < 0x20 || ( value >= 0x7F && value < 0xA0 ) ) {
			return 0;
		}

		if ( ( value >= 0x0300 && value <= 0x036F ) ||
			( value >= 0x0483 && value <= 0x0489 ) ||
			( value >= 0x0591 && value <= 0x05BD ) ||
			( value >= 0x0610 && value <= 0x061A ) ||
			( value >= 0x064B && value <= 0x065F ) ||
			( value >= 0x0670 && value <= 0x0670 ) ||
			( value >= 0x06D6 && value <= 0x06DC ) ||
			( value >= 0x0E31 && value <= 0x0E3A ) ||
			( value >= 0x0E47 && value <= 0x0E4E ) ||
			( value >= 0x1AB0 && value <= 0x1AFF ) ||
			( value >= 0x1DC0 && value <= 0x1DFF ) ||
			( value >= 0x20D0 && value <= 0x20FF ) ||
			( value >= 0xFE00 && value <= 0xFE0F ) ||
			( value >= 0xFE20 && value <= 0xFE2F ) ||
			value == 0x200B || value == 0x200C || value == 0x200D ||
			value == 0xFEFF ) {
			return 0;
		}

		if ( is_emoji_presentation( value ) || is_skin_tone_modifier( value ) ||
			is_regional_indicator( value ) ) {
			return 2;
		}

		if ( is_wide_codepoint( value ) ) {
			return 2;
		}

		if ( ambiguous_width == 2 && is_ambiguous_codepoint( value ) ) {
			return 2;
		}

		return 1;
	}

	auto next_cluster( const std::string_view text, const std::size_t ambiguous_width )
		-> std::pair< std::string_view, std::size_t > {
		if ( text.empty( ) ) {
			return { { }, 0 };
		}

		auto offset = std::size_t{ 0 };
		auto width = std::size_t{ 0 };
		auto seen_base = false;
		auto after_joiner = false;
		auto last_codepoint = char32_t{ 0 };
		auto regional_pair = false;

		while ( offset < text.size( ) ) {
			const auto decoded = decode_utf8( text.substr( offset ) );
			const auto value = decoded.codepoint;

			// One invalid byte is its own cluster; it never joins a base.
			if ( value == 0xFFFD && decoded.length == 1 && !seen_base ) {
				return { text.substr( 0, 1 ), 1 };
			}

			if ( value == 0xFFFD && decoded.length == 1 ) {
				break;
			}

			if ( !seen_base ) {
				width = codepoint_width( value, ambiguous_width );
				regional_pair = is_regional_indicator( value );
				seen_base = true;
				last_codepoint = value;
				offset += decoded.length;

				continue;
			}

			// A ZWJ binds the next codepoint into this cluster: an emoji
			// sequence continues through every joined base.
			if ( last_codepoint == 0x200D ) {
				after_joiner = true;
			}

			// Extending members of the cluster: combining marks, joiners,
			// variation selectors, skin tones, keycap marks.
			const auto extends = codepoint_width( value, ambiguous_width ) == 0 ||
				is_skin_tone_modifier( value ) || is_keycap_mark( value ) ||
				after_joiner;

			// A regional-indicator pair is one flag, two columns.
			if ( regional_pair && is_regional_indicator( value ) &&
				width == 2 && last_codepoint + 1 == value ) {
				last_codepoint = value;
				offset += decoded.length;

				break;
			}

			if ( extends ) {
				after_joiner = value != 0x200D && after_joiner;
				last_codepoint = value;
				offset += decoded.length;

				continue;
			}

			break;
		}

		return { text.substr( 0, offset ), width };
	}

	auto string_width( const std::string_view text, const std::size_t ambiguous_width )
		-> std::size_t {
		auto total = std::size_t{ 0 };
		auto rest = text;

		while ( !rest.empty( ) ) {
			const auto [ cluster, width ] = next_cluster( rest, ambiguous_width );

			if ( cluster.empty( ) ) {
				break;
			}

			total += width;
			rest.remove_prefix( cluster.size( ) );
		}

		return total;
	}

	auto truncate_to_width( const std::string_view text, const std::size_t max_width,
		const std::size_t ambiguous_width ) -> std::string {
		auto total = std::size_t{ 0 };
		auto rest = text;

		while ( !rest.empty( ) ) {
			const auto [ cluster, width ] = next_cluster( rest, ambiguous_width );

			if ( cluster.empty( ) ) {
				break;
			}

			if ( total + width > max_width ) {
				break;
			}

			total += width;
			rest.remove_prefix( cluster.size( ) );
		}

		return std::string{ text.substr( 0, text.size( ) - rest.size( ) ) };
	}

	auto split_lines( const std::string_view text ) -> std::vector< std::string > {
		auto out = std::vector< std::string >{ };
		auto start = std::size_t{ 0 };

		while ( start < text.size( ) ) {
			auto newline = text.find( '\n', start );

			if ( newline == std::string_view::npos ) {
				newline = text.size( );
			}

			auto line = text.substr( start, newline - start );

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.remove_suffix( 1 );
			}

			out.push_back( std::string{ line } );

			if ( newline == text.size( ) ) {
				break;
			}

			start = newline + 1;
		}

		return out;
	}

	cell_buffer::cell_buffer( const std::size_t row_count, const std::size_t column_count,
		const std::size_t ambiguous_width )
		: rows_( row_count ), columns_( column_count ), ambiguous_width_( ambiguous_width ),
		cells_( row_count * column_count ) { }

	auto cell_buffer::resize( const std::size_t row_count, const std::size_t column_count )
		-> void {
		rows_ = row_count;
		columns_ = column_count;
		cells_.assign( row_count * column_count, cell{ } );
	}

	auto cell_buffer::clear( ) -> void {
		std::fill( cells_.begin( ), cells_.end( ), cell{ } );
	}

	auto cell_buffer::at( const std::size_t row, const std::size_t column ) const -> const cell& {
		return cells_[ row * columns_ + column ];
	}

	auto cell_buffer::at( const std::size_t row, const std::size_t column ) -> cell& {
		return cells_[ row * columns_ + column ];
	}

	auto cell_buffer::set_cluster( const std::size_t row, const std::size_t column,
		const std::string_view cluster_text, const std::size_t cluster_width,
		const style& value ) -> std::size_t {
		if ( row >= rows_ || column >= columns_ || cluster_width == 0 ||
			cluster_width > columns_ - column ) {
			return column;
		}

		auto& base = at( row, column );
		base.text = std::string{ cluster_text };
		base.cell_style = value;
		base.width = static_cast< std::uint8_t >( cluster_width );

		for ( auto index = std::size_t{ 1 }; index < cluster_width; ++index ) {
			auto& continuation = at( row, column + index );
			continuation.text.clear( );
			continuation.cell_style = value;
			continuation.width = 0;
		}

		return column + cluster_width;
	}

	auto cell_buffer::write_text( const std::size_t row, std::size_t column,
		const std::string_view text, const style& value ) -> std::size_t {
		auto rest = text;

		while ( !rest.empty( ) ) {
			const auto [ cluster, width ] = next_cluster( rest, ambiguous_width_ );

			if ( cluster.empty( ) ) {
				break;
			}

			if ( column >= columns_ ) {
				break;
			}

			if ( width > columns_ - column ) {
				break;
			}

			column = set_cluster( row, column, cluster, width, value );
			rest.remove_prefix( cluster.size( ) );
		}

		return column;
	}

	auto cell_buffer::write_line( const std::size_t row, const styled_line& line )
		-> std::size_t {
		auto column = std::size_t{ 0 };

		for ( const auto& value : line ) {
			column = write_text( row, column, value.text, span_style( value ) );
		}

		return column;
	}

}
