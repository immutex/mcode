#include "mcode/tui/markdown.hxx"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcode::tui {

	auto render_inline( const std::string_view text, const token base ) -> styled_line {
		auto out = styled_line{ };
		auto plain = std::string{ };
		auto index = std::size_t{ 0 };

		const auto flush_plain = [&]( ) {
			if ( !plain.empty( ) ) {
				out.push_back( { std::move( plain ), base } );
				plain.clear( );
			}
		};

		while ( index < text.size( ) ) {
			const auto character = text[ index ];

			if ( character == '`' ) {
				const auto close = text.find( '`', index + 1 );

				if ( close != std::string_view::npos && close > index + 1 ) {
					flush_plain( );

					auto code = std::string{ text.substr( index + 1, close - index - 1 ) };
					out.push_back( { std::move( code ), token::warn, token::none, false,
						false, true } );

					index = close + 1;

					continue;
				}
			}

			if ( character == '*' && index + 1 < text.size( ) && text[ index + 1 ] == '*' ) {
				const auto close = text.find( "**", index + 2 );

				if ( close != std::string_view::npos && close > index + 2 ) {
					flush_plain( );

					auto bold_text = std::string{ text.substr( index + 2, close - index - 2 ) };
					out.push_back( { std::move( bold_text ), base, token::none, true } );

					index = close + 2;

					continue;
				}
			}

			if ( character == '*' ) {
				const auto close = text.find( '*', index + 1 );

				if ( close != std::string_view::npos && close > index + 1 ) {
					flush_plain( );

					auto italic_text = std::string{ text.substr( index + 1, close - index - 1 ) };
					out.push_back( { std::move( italic_text ), base, token::none, false, true } );

					index = close + 1;

					continue;
				}
			}

			plain.push_back( character );
			++index;
		}

		flush_plain( );

		return out;
	}

	namespace {

		inline constexpr std::string_view HORIZONTAL_RULE = "─";
		inline constexpr std::string_view QUOTE_GUTTER = "│ ";
		inline constexpr std::string_view UNORDERED_BULLET = "• ";

		// The block pass never measures display width, so these cap the
		// emitted glyphs, not the columns.
		inline constexpr std::size_t MAX_RULE_GLYPHS = 80;
		inline constexpr std::size_t MAX_INDENT_LEVELS = 4;
		inline constexpr std::size_t MAX_QUOTE_GLYPHS = 1;
		inline constexpr std::size_t MAX_ORDER_GLYPHS = 4;
		inline constexpr std::size_t MAX_HEADING_LEVEL = 6;

		inline constexpr std::string_view INDENT_UNIT = "  ";

		auto is_space( const char value ) noexcept -> bool {
			return value == ' ' || value == '\t';
		}

		auto repeat( const std::string_view unit, const std::size_t count ) -> std::string {
			auto out = std::string{ };
			out.reserve( unit.size( ) * count );

			for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
				out += unit;
			}

			return out;
		}

		auto strip_leading( const std::string_view line ) noexcept -> std::string_view {
			auto start = std::size_t{ 0 };

			while ( start < line.size( ) && is_space( line[ start ] ) ) {
				++start;
			}

			return line.substr( start );
		}

		auto rule_run( const std::string_view text, const char glyph ) noexcept -> bool {
			if ( text.size( ) < 3 ) {
				return false;
			}

			for ( const auto character : text ) {
				if ( character != glyph && character != ' ' ) {
					return false;
				}
			}

			return true;
		}

		auto is_horizontal_rule( const std::string_view line ) noexcept -> bool {
			const auto body = strip_leading( line );

			return rule_run( body, '-' ) || rule_run( body, '*' ) || rule_run( body, '_' );
		}

		auto is_fence( const std::string_view line ) noexcept -> bool {
			const auto body = strip_leading( line );

			return body.size( ) >= 3 && body[ 0 ] == '`' && body[ 1 ] == '`' && body[ 2 ] == '`';
		}

		auto heading_level( const std::string_view line ) noexcept -> std::size_t {
			const auto body = strip_leading( line );
			auto level = std::size_t{ 0 };

			while ( level < body.size( ) && body[ level ] == '#' &&
				level < MAX_HEADING_LEVEL ) {
				++level;
			}

			if ( level == 0 || level >= body.size( ) || body[ level ] != ' ' ) {
				return 0;
			}

			return level;
		}

		auto unordered_body( const std::string_view body ) noexcept -> std::string_view {
			if ( body.size( ) < 2 ) {
				return { };
			}

			const auto marker = body[ 0 ];

			if ( ( marker == '-' || marker == '*' || marker == '+' ) && body[ 1 ] == ' ' ) {
				return body.substr( 2 );
			}

			return { };
		}

		auto ordered_body( const std::string_view body ) noexcept -> std::string_view {
			auto index = std::size_t{ 0 };

			while ( index < body.size( ) && body[ index ] >= '0' && body[ index ] <= '9' ) {
				++index;
			}

			if ( index == 0 || index >= body.size( ) ) {
				return { };
			}

			if ( ( body[ index ] == '.' || body[ index ] == ')' ) &&
				index + 1 < body.size( ) && body[ index + 1 ] == ' ' ) {
				return body.substr( index + 2 );
			}

			return { };
		}

		auto quote_body( const std::string_view body ) noexcept -> std::string_view {
			if ( !body.empty( ) && body[ 0 ] == '>' ) {
				return body.substr( 1 );
			}

			return { };
		}

		// Leading spaces indent a block by whole two-space levels, a tab by one;
		// anything else in the run ends the indent.
		auto indent_levels( const std::string_view line ) noexcept -> std::size_t {
			auto index = std::size_t{ 0 };
			auto levels = std::size_t{ 0 };

			while ( index < line.size( ) ) {
				if ( line[ index ] == '\t' ) {
					++levels;
					++index;
				} else if ( line[ index ] == ' ' && index + 1 < line.size( ) &&
					line[ index + 1 ] == ' ' ) {
					++levels;
					index += 2;
				} else {
					break;
				}
			}

			if ( levels > MAX_INDENT_LEVELS ) {
				levels = MAX_INDENT_LEVELS;
			}

			return levels;
		}

		auto is_blank( const std::string_view line ) noexcept -> bool {
			for ( const auto character : line ) {
				if ( !is_space( character ) ) {
					return false;
				}
			}

			return true;
		}

		auto trim_trailing( const std::string_view line ) noexcept -> std::string_view {
			auto end = line.size( );

			while ( end > 0 && is_space( line[ end - 1 ] ) ) {
				--end;
			}

			return line.substr( 0, end );
		}

		auto rule_line( ) -> styled_line {
			return styled_line{ { repeat( HORIZONTAL_RULE, MAX_RULE_GLYPHS ), token::muted } };
		}

		auto code_row( const std::string_view text, const token base ) -> styled_line {
			auto row = styled_line{ };

			if ( !text.empty( ) ) {
				row.push_back( { std::string{ text }, base, token::code_bg } );
			}

			return row;
		}

		auto heading_row( const std::string_view body, const std::size_t level,
			const token base ) -> styled_line {
			auto row = styled_line{ };

			for ( auto marker = std::size_t{ 0 }; marker < level; ++marker ) {
				row.push_back( { std::string{ "#" }, base, token::none, false, false, true } );
			}

			auto content = render_inline( body, base );

			for ( auto& span : content ) {
				span.color = token::accent;
				span.bold = true;
				span.dim = false;
			}

			for ( auto& span : content ) {
				row.push_back( std::move( span ) );
			}

			return row;
		}

		auto list_row( const std::string_view marker, const std::string_view body,
			const token base, const std::size_t level ) -> styled_line {
			auto row = styled_line{ };

			if ( level > 0 ) {
				row.push_back( { repeat( INDENT_UNIT, level ), base } );
			}

			row.push_back( { std::string{ marker }, token::muted } );

			auto content = render_inline( body, base );

			for ( auto& span : content ) {
				row.push_back( std::move( span ) );
			}

			return row;
		}

		auto quote_row( const std::string_view body, const token base ) -> styled_line {
			auto row = styled_line{ };

			row.push_back( { repeat( QUOTE_GUTTER, MAX_QUOTE_GLYPHS ), token::muted } );

			auto content = render_inline( body, base );

			for ( auto& span : content ) {
				span.dim = true;
				span.color = base;
			}

			for ( auto& span : content ) {
				row.push_back( std::move( span ) );
			}

			return row;
		}

	}

	auto render_markdown( const std::string_view text, const token base )
		-> std::vector< styled_line > {
		auto rows = std::vector< styled_line >{ };

		if ( text.empty( ) ) {
			return rows;
		}

		const auto emit = [ & ]( styled_line line ) {
			if ( line.empty( ) ) {
				line.push_back( { std::string{ }, base } );
			}

			rows.push_back( std::move( line ) );
		};

		const auto emit_indent = [ & ]( const std::string_view line ) {
			auto row = styled_line{ };
			const auto levels = indent_levels( line );

			if ( levels > 0 ) {
				row.push_back( { repeat( INDENT_UNIT, levels ), base } );
			}

			auto content = render_inline( strip_leading( line ), base );

			for ( auto& span : content ) {
				row.push_back( std::move( span ) );
			}

			emit( std::move( row ) );
		};

		auto in_code = false;
		auto index = std::size_t{ 0 };

		while ( index < text.size( ) ) {
			const auto newline = text.find( '\n', index );
			const auto stop = newline == std::string_view::npos ? text.size( ) : newline;
			const auto raw = text.substr( index, stop - index );
			auto line = raw;

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.remove_suffix( 1 );
			}

			if ( is_fence( line ) ) {
				in_code = !in_code;
				index = stop + 1;

				continue;
			}

			if ( in_code ) {
				emit( code_row( line, base ) );
				index = stop + 1;

				continue;
			}

			if ( is_blank( line ) ) {
				emit( styled_line{ } );
				index = stop + 1;

				continue;
			}

			if ( is_horizontal_rule( line ) ) {
				emit( rule_line( ) );
				index = stop + 1;

				continue;
			}

			const auto levels = indent_levels( line );
			const auto body = strip_leading( line );
			const auto heading = heading_level( line );

			if ( heading > 0 ) {
				emit( heading_row( strip_leading( body.substr( heading + 1 ) ), heading, base ) );
				index = stop + 1;

				continue;
			}

			if ( !body.empty( ) && body[ 0 ] == '>' ) {
				emit( quote_row( trim_trailing( quote_body( body ) ), base ) );
				index = stop + 1;

				continue;
			}

			if ( const auto item = unordered_body( body ); !item.empty( ) ) {
				emit( list_row( UNORDERED_BULLET, item, base, levels ) );
				index = stop + 1;

				continue;
			}

			if ( const auto item = ordered_body( body ); !item.empty( ) ) {
				const auto number = body.substr( 0, body.size( ) - item.size( ) - 2 );
				auto marker = std::string{ };

				if ( number.size( ) < MAX_ORDER_GLYPHS ) {
					marker.append( MAX_ORDER_GLYPHS - number.size( ), ' ' );
				}

				marker += number;
				marker += ". ";
				emit( list_row( marker, item, base, levels ) );
				index = stop + 1;

				continue;
			}

			emit_indent( line );
			index = stop + 1;
		}

		return rows;
	}

}
