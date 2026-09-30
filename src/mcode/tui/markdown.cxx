#include "mcode/tui/markdown.hxx"

namespace mcode::tui {

	namespace {

		constexpr std::size_t FENCE_MARKER_MINIMUM = 3;

		[[nodiscard]] auto is_fence( const std::string_view line ) -> bool {
			return line.size( ) >= FENCE_MARKER_MINIMUM &&
				line.substr( 0, FENCE_MARKER_MINIMUM ) == "```";
		}

		[[nodiscard]] auto fence_info( const std::string_view line ) -> std::string_view {
			auto rest = line.substr( FENCE_MARKER_MINIMUM );

			while ( !rest.empty( ) && rest.front( ) == ' ' ) {
				rest.remove_prefix( 1 );
			}

			while ( !rest.empty( ) && rest.back( ) == ' ' ) {
				rest.remove_suffix( 1 );
			}

			return rest;
		}

		[[nodiscard]] auto heading_level( const std::string_view line ) -> std::size_t {
			auto level = std::size_t{ 0 };

			while ( level < line.size( ) && line[ level ] == '#' ) {
				++level;
			}

			if ( level == 0 || level > 6 || level >= line.size( ) ||
				line[ level ] != ' ' ) {
				return 0;
			}

			return level;
		}

		[[nodiscard]] auto heading_text( const std::string_view line,
			const std::size_t level ) -> std::string_view {
			auto rest = line.substr( level );

			while ( !rest.empty( ) && rest.front( ) == ' ' ) {
				rest.remove_prefix( 1 );
			}

			return rest;
		}

		[[nodiscard]] auto list_marker( const std::string_view line ) -> std::size_t {
			auto spaces = std::size_t{ 0 };

			while ( spaces < line.size( ) && line[ spaces ] == ' ' ) {
				++spaces;
			}

			const auto rest = line.substr( spaces );

			if ( rest.starts_with( "- " ) || rest.starts_with( "* " ) ) {
				return spaces + 2;
			}

			return 0;
		}

	}

	auto markdown_parser::close_open( ) -> void {
		if ( !blocks_.empty( ) ) {
			blocks_.back( ).open = false;
		}
	}

	auto markdown_parser::start_block( const md_block::kind type, const std::size_t level,
		const std::string_view info ) -> void {
		auto block = md_block{ };
		block.type = type;
		block.level = level;
		block.info = std::string{ info };
		block.open = true;

		blocks_.push_back( std::move( block ) );
	}

	auto markdown_parser::feed( const std::string_view chunk ) -> std::vector< md_block > {
		if ( stream_done_ ) {
			return { };
		}

		carry_.append( chunk );

		auto changed = std::vector< md_block >{ };
		auto start = std::size_t{ 0 };

		while ( start <= carry_.size( ) ) {
			const auto newline = carry_.find( '\n', start );

			if ( newline == std::string::npos ) {
				break;
			}

			const auto line = std::string_view{ carry_ }.substr( start, newline - start );
			const auto trimmed = line.empty( ) || ( line.size( ) == 1 && line.back( ) == '\r' );

			if ( trimmed ) {
				// A blank line closes the open block.
				if ( !blocks_.empty( ) && blocks_.back( ).open ) {
					close_open( );
					changed.push_back( blocks_.back( ) );
				}

				start = newline + 1;

				continue;
			}

			auto body = line;

			if ( body.back( ) == '\r' ) {
				body.remove_suffix( 1 );
			}

			if ( is_fence( body ) ) {
				const auto info = fence_info( body );

				if ( !blocks_.empty( ) && blocks_.back( ).open &&
					blocks_.back( ).type == md_block::kind::fenced_code ) {
					close_open( );
					changed.push_back( blocks_.back( ) );
				} else {
					if ( !blocks_.empty( ) && blocks_.back( ).open ) {
						close_open( );
						changed.push_back( blocks_.back( ) );
					}

					start_block( md_block::kind::fenced_code, 0, info );
					changed.push_back( blocks_.back( ) );
				}

				start = newline + 1;

				continue;
			}

			if ( !blocks_.empty( ) && blocks_.back( ).open &&
				blocks_.back( ).type == md_block::kind::fenced_code ) {
				auto& block = blocks_.back( );
				auto code_style = style{ };
				code_style.foreground = token::text;
				code_style.background = token::code_bg;

				block.lines.push_back( { { std::string{ body }, token::text, token::code_bg } } );
				changed.push_back( block );

				start = newline + 1;

				continue;
			}

			if ( const auto level = heading_level( body ); level > 0 ) {
				if ( !blocks_.empty( ) && blocks_.back( ).open ) {
					close_open( );
					changed.push_back( blocks_.back( ) );
				}

				start_block( md_block::kind::heading, level, { } );

				auto& block = blocks_.back( );
				auto heading_style = style{ };
				heading_style.foreground = token::accent;
				heading_style.bold = true;

				auto line_spans = styled_line{ };
				line_spans.push_back( { std::string{ heading_text( body, level ) },
					token::accent, token::none, true, false, false } );

				block.lines.push_back( std::move( line_spans ) );
				close_open( );
				changed.push_back( blocks_.back( ) );

				start = newline + 1;

				continue;
			}

			if ( const auto marker = list_marker( body ); marker > 0 ) {
				if ( !blocks_.empty( ) && blocks_.back( ).open &&
					blocks_.back( ).type != md_block::kind::list_item ) {
					close_open( );
					changed.push_back( blocks_.back( ) );
				}

				if ( blocks_.empty( ) || !blocks_.back( ).open ) {
					start_block( md_block::kind::list_item, 0, { } );
				}

				auto& block = blocks_.back( );
				auto item = std::string{ "  " };
				item += body.substr( marker - 2, 2 );
				item += ' ';

				auto line_spans = styled_line{ };
				line_spans.push_back( { item, token::muted } );
				line_spans.push_back( { std::string{ body.substr( marker ) }, token::text } );

				block.lines.push_back( std::move( line_spans ) );
				changed.push_back( block );

				start = newline + 1;

				continue;
			}

			if ( blocks_.empty( ) || !blocks_.back( ).open ) {
				start_block( md_block::kind::paragraph, 0, { } );
			}

			auto& block = blocks_.back( );
			auto inline_spans = render_inline( body, token::text );
			block.lines.push_back( std::move( inline_spans ) );
			changed.push_back( block );

			start = newline + 1;
		}

		carry_.erase( 0, start );

		return changed;
	}

	auto markdown_parser::finish( ) -> std::vector< md_block > {
		if ( stream_done_ ) {
			return { };
		}

		stream_done_ = true;

		if ( carry_.empty( ) ) {
			return { };
		}

		auto body = std::string_view{ carry_ };

		if ( body.back( ) == '\n' ) {
			body.remove_suffix( 1 );
		}

		if ( !body.empty( ) && body.back( ) == '\r' ) {
			body.remove_suffix( 1 );
		}

		if ( body.empty( ) ) {
			return { };
		}

		if ( blocks_.empty( ) || !blocks_.back( ).open ) {
			start_block( md_block::kind::paragraph, 0, { } );
		}

		auto& block = blocks_.back( );
		block.lines.push_back( render_inline( body, token::text ) );
		close_open( );

		return { block };
	}

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

}
