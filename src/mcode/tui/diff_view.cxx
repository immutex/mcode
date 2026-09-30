#include "mcode/tui/diff_view.hxx"

#include <algorithm>
#include <cctype>

namespace mcode::tui {

	namespace {

		[[nodiscard]] auto is_word( const char character ) noexcept -> bool {
			return std::isalnum( static_cast< unsigned char >( character ) ) != 0 ||
				character == '_';
		}

		[[nodiscard]] auto tokenize_words( const std::string_view line )
			-> std::vector< std::string_view > {
			auto out = std::vector< std::string_view >{ };
			auto index = std::size_t{ 0 };

			while ( index < line.size( ) ) {
				if ( is_word( line[ index ] ) ) {
					const auto start = index;

					while ( index < line.size( ) && is_word( line[ index ] ) ) {
						++index;
					}

					out.push_back( line.substr( start, index - start ) );

					continue;
				}

				++index;
			}

			return out;
		}

		// The longest common subsequence of the two token lists, as a set of
		// matched pairs. Two lines are short; the O(n*m) table is bounded by
		// the line length and this runs once per paired diff line.
		[[nodiscard]] auto token_lcs( const std::vector< std::string_view >& removed,
			const std::vector< std::string_view >& added )
			-> std::vector< std::vector< std::size_t > > {
			auto table = std::vector< std::vector< std::size_t > >(
				removed.size( ) + 1, std::vector< std::size_t >( added.size( ) + 1, 0 ) );

			for ( auto left = removed.size( ); left > 0; --left ) {
				for ( auto right = added.size( ); right > 0; --right ) {
					if ( removed[ left - 1 ] == added[ right - 1 ] ) {
						table[ left - 1 ][ right - 1 ] = table[ left ][ right ] + 1;
					} else {
						table[ left - 1 ][ right - 1 ] =
							std::max( table[ left ][ right - 1 ], table[ left - 1 ][ right ] );
					}
				}
			}

			return table;
		}

		// The token spans of `line` that are NOT part of the LCS with `other`,
		// i.e. the changed words.
		[[nodiscard]] auto changed_tokens( const std::string_view line,
			const std::string_view other ) -> std::vector< std::pair< std::size_t, std::size_t > > {
			const auto own = tokenize_words( line );
			const auto foreign = tokenize_words( other );
			const auto table = token_lcs( own, foreign );

			auto matched = std::vector< bool >( own.size( ), false );
			auto left = std::size_t{ 0 };
			auto right = std::size_t{ 0 };

			while ( left < own.size( ) && right < foreign.size( ) ) {
				if ( own[ left ] == foreign[ right ] ) {
					matched[ left ] = true;
					++left;
					++right;

					continue;
				}

				if ( table[ left + 1 ][ right ] >= table[ left ][ right + 1 ] ) {
					++left;
				} else {
					++right;
				}
			}

			auto out = std::vector< std::pair< std::size_t, std::size_t > >{ };
			auto cursor = std::size_t{ 0 };
			auto token_index = std::size_t{ 0 };

			while ( cursor < line.size( ) && token_index < own.size( ) ) {
				const auto at = line.find( own[ token_index ], cursor );

				if ( at == std::string_view::npos ) {
					++token_index;

					continue;
				}

				if ( !matched[ token_index ] ) {
					out.push_back( { at, at + own[ token_index ].size( ) } );
				}

				cursor = at + own[ token_index ].size( );
				++token_index;
			}

			return out;
		}

		[[nodiscard]] auto line_kind( const std::string_view line ) -> diff_line::kind {
			if ( line.starts_with( "+" ) && !line.starts_with( "+++" ) ) {
				return diff_line::kind::added;
			}

			if ( line.starts_with( "-" ) && !line.starts_with( "---" ) ) {
				return diff_line::kind::removed;
			}

			return diff_line::kind::context;
		}

		[[nodiscard]] auto gutter( const diff_line::kind type ) -> std::string {
			switch ( type ) {
				case diff_line::kind::added: return "+ ";
				case diff_line::kind::removed: return "- ";
				case diff_line::kind::context: return "  ";
			}

			return "  ";
		}

	}

	auto render_diff( const std::string_view diff_text, const token base )
		-> std::vector< diff_line > {
		const auto lines = split_lines( diff_text );
		auto out = std::vector< diff_line >{ };

		auto removed_index = std::optional< std::size_t >{ };

		for ( auto index = std::size_t{ 0 }; index < lines.size( ); ++index ) {
			const auto& raw = lines[ index ];
			const auto type = line_kind( raw );

			if ( type == diff_line::kind::removed ) {
				removed_index = index;

				auto entry = diff_line{ };
				entry.type = type;
				entry.text = gutter( type ) + raw.substr( 1 );
				entry.spans.push_back( { gutter( type ), token::error } );
				entry.spans.push_back( { std::string{ raw.substr( 1 ) }, token::error } );

				out.push_back( std::move( entry ) );

				continue;
			}

			if ( type == diff_line::kind::added ) {
				auto entry = diff_line{ };
				entry.type = type;
				entry.text = gutter( type ) + raw.substr( 1 );

				// A paired -/+ within the threshold gets word-level emphasis
				// on the changed tokens; an unpaired added line is uniform.
				if ( removed_index && index - *removed_index <= PAIR_THRESHOLD ) {
					entry.spans = emphasize_added( lines[ *removed_index ].substr( 1 ),
						raw.substr( 1 ), token::success );
					removed_index.reset( );
				} else {
					entry.spans.push_back( { gutter( type ), token::success } );
					entry.spans.push_back( { std::string{ raw.substr( 1 ) }, token::success } );
				}

				out.push_back( std::move( entry ) );

				continue;
			}

			removed_index.reset( );

			auto entry = diff_line{ };
			entry.type = type;
			entry.text = gutter( type ) + raw;
			entry.spans.push_back( { gutter( type ), token::muted } );
			entry.spans.push_back( { std::string{ raw }, base } );

			out.push_back( std::move( entry ) );
		}

		return out;
	}

	auto emphasize_added( const std::string_view removed_line,
		const std::string_view added_line, const token base ) -> styled_line {
		const auto changed = changed_tokens( added_line, removed_line );
		auto out = styled_line{ };

		out.push_back( { std::string{ gutter( diff_line::kind::added ) }, token::success } );

		auto cursor = std::size_t{ 0 };

		for ( const auto& [ start, end ] : changed ) {
			if ( start > cursor ) {
				out.push_back( { std::string{ added_line.substr( cursor, start - cursor ) },
					base, token::diff_add_bg } );
			}

			out.push_back( { std::string{ added_line.substr( start, end - start ) },
				base, token::diff_add_emph, true } );

			cursor = end;
		}

		if ( cursor < added_line.size( ) ) {
			out.push_back( { std::string{ added_line.substr( cursor ) },
				base, token::diff_add_bg } );
		}

		return out;
	}

	auto fold_context( std::vector< diff_line > lines ) -> std::vector< diff_line > {
		auto out = std::vector< diff_line >{ };
		auto run = std::vector< diff_line >{ };

		const auto flush = [&]( ) {
			if ( run.size( ) < FOLD_THRESHOLD ) {
				for ( auto& entry : run ) {
					out.push_back( std::move( entry ) );
				}

				run.clear( );

				return;
			}

			auto folded = diff_line{ };
			folded.type = diff_line::kind::context;
			folded.text = "  ... " + std::to_string( run.size( ) ) + " lines";
			folded.spans.push_back( { folded.text, token::muted } );

			out.push_back( std::move( folded ) );
			run.clear( );
		};

		for ( auto& entry : lines ) {
			if ( entry.type == diff_line::kind::context ) {
				run.push_back( std::move( entry ) );

				continue;
			}

			flush( );
			out.push_back( std::move( entry ) );
		}

		flush( );

		return out;
	}

}
