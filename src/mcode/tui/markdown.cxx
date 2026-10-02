#include "mcode/tui/markdown.hxx"

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

}
