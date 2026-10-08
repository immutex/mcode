#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace tui_test {

	using mcode::tui::styled_line;

	inline constexpr std::size_t ROWS = 4;
	inline constexpr std::size_t COLUMNS = 40;

	[[nodiscard]] inline auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	// the text a frame writes, with the escape sequences removed
	[[nodiscard]] inline auto visible_text( const std::string& bytes ) -> std::string {
		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index < bytes.size( ); ++index ) {
			if ( bytes[ index ] != '\x1b' ) {
				out.push_back( bytes[ index ] );

				continue;
			}

			++index;

			while ( index < bytes.size( ) &&
				!std::isalpha( static_cast< unsigned char >( bytes[ index ] ) ) ) {
				++index;
			}
		}

		return out;
	}

	// A state's rendered rows against a fresh render of the same buffer: a
	// cache that lags shows up as a text difference, which is what this
	// compares.
	[[nodiscard]] inline auto same_rows( const std::vector< styled_line >& left,
		const std::vector< styled_line >& right ) -> bool {
		if ( left.size( ) != right.size( ) ) {
			return false;
		}

		for ( auto index = std::size_t{ 0 }; index < left.size( ); ++index ) {
			if ( row_text( left[ index ] ) != row_text( right[ index ] ) ) {
				return false;
			}
		}

		return true;
	}

	// A terminal, in miniature: it consumes the coordinator's byte stream and
	// tracks the screen that would actually result, scrolls included.
	//
	// `visible_text` cannot stand in for this. It strips the positioning, so a
	// screen that scrolled its header off the top and a screen that kept it
	// yield the same string -- which is exactly how a header that never
	// appeared survived 118 tui assertions. Anything that depends on WHERE the
	// bytes land needs a model that has a cursor.
	//
	// Only the sequences the emitter emits are implemented: cursor motion
	// (`A`/`B`/`H`/`G`), erase (`K`/`J`), SGR and private modes (ignored). A
	// sequence it does not know is ignored rather than mistyped as text.
	class screen_model {
	public:
		screen_model( const std::size_t rows, const std::size_t columns )
			: rows_( rows ), columns_( columns ), cells_( rows * columns, ' ' ) { }

		auto feed( const std::string& bytes ) -> void {
			auto index = std::size_t{ 0 };

			while ( index < bytes.size( ) ) {
				const auto value = bytes[ index ];

				if ( value == '\x1b' ) {
					index = escape( bytes, index );

					continue;
				}

				if ( value == '\r' ) {
					cursor_column_ = 0;
					++index;

					continue;
				}

				if ( value == '\n' ) {
					line_feed( );
					++index;

					continue;
				}

				put( value );
				++index;
			}
		}

		// Trailing blanks are stripped: every row is space-filled to its width,
		// so a comparison that kept them would fail on padding alone.
		[[nodiscard]] auto row( const std::size_t index ) const -> std::string {
			if ( index >= rows_ ) {
				return { };
			}

			auto text = std::string{ };

			for ( auto column = std::size_t{ 0 }; column < columns_; ++column ) {
				text.push_back( cells_[ index * columns_ + column ] );
			}

			while ( !text.empty( ) && text.back( ) == ' ' ) {
				text.pop_back( );
			}

			return text;
		}

		// The rows, with every trailing blank row dropped.
		[[nodiscard]] auto rows( ) const -> std::vector< std::string > {
			auto out = std::vector< std::string >{ };

			for ( auto index = std::size_t{ 0 }; index < rows_; ++index ) {
				out.push_back( row( index ) );
			}

			while ( !out.empty( ) && out.back( ).empty( ) ) {
				out.pop_back( );
			}

			return out;
		}

		// How many times the screen scrolled. A scroll is not a defect by
		// itself -- `commit` scrolls on purpose -- but a scroll at a point
		// where the caller believed the screen was static is.
		[[nodiscard]] auto scroll_count( ) const noexcept -> std::size_t {
			return scrolls_;
		}

		[[nodiscard]] auto cursor_row( ) const noexcept -> std::size_t {
			return cursor_row_;
		}

		// The first row holding `needle`, or nothing.
		[[nodiscard]] auto find_row( const std::string_view needle ) const
			-> std::size_t {
			for ( auto index = std::size_t{ 0 }; index < rows_; ++index ) {
				if ( row( index ).find( needle ) != std::string::npos ) {
					return index;
				}
			}

			return rows_;
		}

	private:
		// Returns the index just past the escape sequence starting at `start`.
		auto escape( const std::string& bytes, const std::size_t start ) -> std::size_t {
			if ( start + 1 >= bytes.size( ) ) {
				return start + 1;
			}

			if ( bytes[ start + 1 ] != '[' ) {
				// A two-byte escape (`ESC 7`, `ESC c`): consume both.
				return start + 2;
			}

			auto index = start + 2;
			auto parameters = std::string{ };

			while ( index < bytes.size( ) ) {
				const auto value = bytes[ index ];

				if( std::isdigit( static_cast< unsigned char >( value ) ) ||
					value == ';' || value == '?' ) {
					parameters.push_back( value );
					++index;

					continue;
				}

				break;
			}

			if ( index >= bytes.size( ) ) {
				return index;
			}

			apply( parameters, bytes[ index ] );

			return index + 1;
		}

		// The first parameter, or `fallback` when the sequence omits it -- a
		// bare `\x1b[A` means one row, not zero.
		[[nodiscard]] auto parameter( const std::string& parameters,
			const std::size_t position, const std::size_t fallback ) const -> std::size_t {
			auto value = std::size_t{ 0 };
			auto seen = false;
			auto index = std::size_t{ 0 };
			auto field = std::size_t{ 0 };

			while ( index <= parameters.size( ) ) {
				const auto at_end = index == parameters.size( );
				const auto value_at = at_end ? ';' : parameters[ index ];

				if ( value_at == ';' ) {
					if ( field == position ) {
						return seen ? value : fallback;
					}

					++field;
					value = 0;
					seen = false;
					++index;

					continue;
				}

				if ( std::isdigit( static_cast< unsigned char >( value_at ) ) ) {
					value = value * 10 + static_cast< std::size_t >( value_at - '0' );
					seen = true;
				}

				++index;
			}

			return fallback;
		}

		auto apply( const std::string& parameters, const char final ) -> void {
			switch ( final ) {
				case 'A': {
					const auto count = parameter( parameters, 0, 1 );

					cursor_row_ = cursor_row_ > count ? cursor_row_ - count : 0;

					break;
				}

				case 'B': {
					const auto count = parameter( parameters, 0, 1 );

					cursor_row_ = cursor_row_ + count < rows_ ? cursor_row_ + count
						: rows_ - 1;

					break;
				}

				case 'H':
				case 'f': {
					const auto row = parameter( parameters, 0, 1 );
					const auto column = parameter( parameters, 1, 1 );

					cursor_row_ = row > 0 && row <= rows_ ? row - 1 : cursor_row_;
					cursor_column_ = column > 0 && column <= columns_ ? column - 1
						: cursor_column_;

					break;
				}

				case 'G': {
					const auto column = parameter( parameters, 0, 1 );

					cursor_column_ = column > 0 && column <= columns_ ? column - 1 : 0;

					break;
				}

				case 'K': {
					const auto mode = parameter( parameters, 0, 0 );
					const auto from = mode == 1 || mode == 2 ? std::size_t{ 0 }
						: cursor_column_;
					const auto to = mode == 0 ? columns_ : cursor_column_ + 1;

					for ( auto column = from; column < to && column < columns_; ++column ) {
						cells_[ cursor_row_ * columns_ + column ] = ' ';
					}

					break;
				}

				case 'J': {
					const auto mode = parameter( parameters, 0, 0 );

					if ( mode == 2 ) {
						cells_.assign( rows_ * columns_, ' ' );

						break;
					}

					const auto from = mode == 1 ? std::size_t{ 0 }
						: cursor_row_ * columns_ + cursor_column_;
					const auto to = mode == 0 ? rows_ * columns_
						: cursor_row_ * columns_ + cursor_column_ + 1;

					for ( auto at = from; at < to && at < cells_.size( ); ++at ) {
						cells_[ at ] = ' ';
					}

					break;
				}

				default:
					// SGR (`m`), private modes (`h`/`l`) and the rest carry no
					// cursor or cell change.
					break;
			}
		}

		auto line_feed( ) -> void {
			if ( cursor_row_ + 1 < rows_ ) {
				++cursor_row_;

				return;
			}

			// At the bottom row: a real terminal scrolls, and so does this. The
			// top row is lost, which is the whole point of modelling it.
			for ( auto index = std::size_t{ 0 }; index + 1 < rows_; ++index ) {
				for ( auto column = std::size_t{ 0 }; column < columns_; ++column ) {
					cells_[ index * columns_ + column ] =
						cells_[ ( index + 1 ) * columns_ + column ];
				}
			}

			for ( auto column = std::size_t{ 0 }; column < columns_; ++column ) {
				cells_[ ( rows_ - 1 ) * columns_ + column ] = ' ';
			}

			++scrolls_;
		}

		auto put( const char value ) -> void {
			if ( cursor_row_ >= rows_ || cursor_column_ >= columns_ ) {
				return;
			}

			cells_[ cursor_row_ * columns_ + cursor_column_ ] = value;
			++cursor_column_;

			// DECAWM is on by default: a write past the last column wraps. The
			// emitter reserves one column to avoid relying on this, but a model
			// that ignored the wrap would disagree with a real terminal.
			if ( cursor_column_ >= columns_ ) {
				cursor_column_ = 0;
				line_feed( );
			}
		}

		std::size_t rows_;
		std::size_t columns_;
		std::vector< char > cells_;
		std::size_t cursor_row_ = 0;
		std::size_t cursor_column_ = 0;
		std::size_t scrolls_ = 0;
	};

} // namespace tui_test
