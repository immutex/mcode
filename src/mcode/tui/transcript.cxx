#include "mcode/tui/transcript.hxx"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"

namespace mcode::tui {

	namespace {

		[[nodiscard]] auto blank_row( const styled_line& row ) -> bool {
			for ( const auto& span : row ) {
				if ( !span.text.empty( ) ) {
					return false;
				}
			}

			return true;
		}

		constexpr auto INDENT_GLYPHS = std::array< std::string_view, 6 >{
			"•", "│", "❯", "✻", "✓", "#",
		};

		// A number reads as an indent only when a whole span is the marker.
		constexpr auto ORDER_GLYPHS = std::array< std::string_view, 12 >{
			"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", ")",
		};

		inline constexpr std::string_view QUOTE_GLYPH = "│";

		struct cluster_ref {
			std::string_view text;
			std::size_t width = 0;
			const styled_span* source = nullptr;
		};

		[[nodiscard]] auto is_space_cluster( const std::string_view cluster ) noexcept -> bool {
			return cluster == " " || cluster == "\t";
		}

		template< std::size_t GlyphCount >
		[[nodiscard]] auto is_glyph( const std::string_view cluster,
			const std::array< std::string_view, GlyphCount >& glyphs ) noexcept -> bool {
			for ( const auto& glyph : glyphs ) {
				if ( cluster == glyph ) {
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] auto is_indent_cluster( const std::string_view cluster ) noexcept -> bool {
			if ( is_space_cluster( cluster ) ) {
				return true;
			}

			return is_glyph( cluster, INDENT_GLYPHS ) || is_glyph( cluster, ORDER_GLYPHS );
		}

		[[nodiscard]] auto is_indent_span( const styled_span& span,
			const std::size_t ambiguous_width ) -> bool {
			auto rest = std::string_view{ span.text };

			while ( !rest.empty( ) ) {
				const auto cluster = next_cluster( rest, ambiguous_width ).first;

				if ( cluster.empty( ) || !is_indent_cluster( cluster ) ) {
					return false;
				}

				rest.remove_prefix( cluster.size( ) );
			}

			return true;
		}

		[[nodiscard]] auto indent_spans( const styled_line& row,
			const std::size_t ambiguous_width ) -> std::size_t {
			auto count = std::size_t{ 0 };

			while ( count < row.size( ) && is_indent_span( row[ count ], ambiguous_width ) ) {
				++count;
			}

			return count < row.size( ) ? count : std::size_t{ 0 };
		}

		[[nodiscard]] auto spans_width( const styled_line& row, const std::size_t count,
			const std::size_t ambiguous_width ) -> std::size_t {
			auto total = std::size_t{ 0 };

			for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
				total += string_width( row[ index ].text, ambiguous_width );
			}

			return total;
		}

		// A gutter keeps its glyph; every other marker becomes blanks of the same width.
		[[nodiscard]] auto continuation_indent( const styled_line& row,
			const std::size_t count, const std::size_t ambiguous_width ) -> styled_line {
			auto out = styled_line{ };
			out.reserve( count );

			for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
				const auto& span = row[ index ];

				if ( span.text.find( QUOTE_GLYPH ) != std::string::npos ) {
					out.push_back( span );

					continue;
				}

				const auto width = string_width( span.text, ambiguous_width );
				out.push_back( styled_span{ std::string( width, ' ' ), span.color,
					span.background, span.bold, span.italic, span.dim } );
			}

			return out;
		}

		[[nodiscard]] auto collect_clusters( const styled_line& row, const std::size_t first,
			const std::size_t first_offset, const std::size_t ambiguous_width )
			-> std::vector< cluster_ref > {
			auto clusters = std::vector< cluster_ref >{ };

			for ( auto index = first; index < row.size( ); ++index ) {
				const auto& span = row[ index ];
				auto rest = std::string_view{ span.text }.substr(
					index == first ? first_offset : 0 );

				while ( !rest.empty( ) ) {
					const auto [ cluster, width ] = next_cluster( rest, ambiguous_width );

					if ( cluster.empty( ) ) {
						break;
					}

					clusters.push_back( cluster_ref{ cluster, width, &span } );
					rest.remove_prefix( cluster.size( ) );
				}
			}

			return clusters;
		}

		struct lead_run {
			std::string text;
			std::size_t width = 0;
		};

		[[nodiscard]] auto measure_lead( const styled_line& row, const std::size_t first,
			const std::size_t ambiguous_width ) -> lead_run {
			if ( first >= row.size( ) ) {
				return { };
			}

			auto run = lead_run{ };
			auto rest = std::string_view{ row[ first ].text };

			while ( !rest.empty( ) ) {
				const auto [ cluster, width ] = next_cluster( rest, ambiguous_width );

				if ( !is_space_cluster( cluster ) ) {
					break;
				}

				run.text += cluster;
				run.width += width;
				rest.remove_prefix( cluster.size( ) );
			}

			return run;
		}

		auto append_clusters( styled_line& line, const std::vector< cluster_ref >& clusters,
			const std::size_t begin, const std::size_t end ) -> void {
			const styled_span* open = nullptr;

			for ( auto index = begin; index < end; ++index ) {
				const auto& cluster = clusters[ index ];
				const auto& source = *cluster.source;

				if ( !line.empty( ) && open == cluster.source ) {
					line.back( ).text.append( cluster.text );

					continue;
				}

				line.push_back( styled_span{ std::string{ cluster.text }, source.color,
					source.background, source.bold, source.italic, source.dim } );
				open = cluster.source;
			}
		}

		// Fenced code is verbatim: `token::code_bg` keys the skip.
		[[nodiscard]] auto needs_wrap( const styled_line& row, const std::size_t columns,
			const std::size_t ambiguous_width ) -> bool {
			for ( const auto& span : row ) {
				if ( span.background == token::code_bg ) {
					return false;
				}
			}

			return spans_width( row, row.size( ), ambiguous_width ) > columns;
		}

		[[nodiscard]] auto split_row( const styled_line& row, const std::size_t columns,
			const std::size_t ambiguous_width ) -> std::vector< styled_line > {
			const auto indent = indent_spans( row, ambiguous_width );
			const auto indent_width = spans_width( row, indent, ambiguous_width );

			const auto hangs = indent_width < columns;
			const auto hang = hangs ? indent_width : std::size_t{ 0 };

			auto lead = lead_run{ };

			if ( hangs ) {
				lead = measure_lead( row, indent, ambiguous_width );

				if ( hang + lead.width >= columns ) {
					lead = lead_run{ };
				}
			}

			const auto prefix_width = hang + lead.width;
			const auto budget = columns - prefix_width;
			const auto clusters = collect_clusters( row, indent, lead.text.size( ),
				ambiguous_width );

			auto head = styled_line{ };
			auto tail = styled_line{ };

			if ( hangs ) {
				head.assign( row.begin( ),
					row.begin( ) + static_cast< std::ptrdiff_t >( indent ) );
				tail = continuation_indent( row, indent, ambiguous_width );

				if ( !lead.text.empty( ) ) {
					const auto& content = row[ indent ];
					head.push_back( styled_span{ lead.text, content.color,
						content.background, content.bold, content.italic, content.dim } );
					tail.push_back( head.back( ) );
				}
			}

			auto rows = std::vector< styled_line >{ };

			if ( !hangs ) {
				rows.push_back( styled_line{ row.begin( ),
					row.begin( ) + static_cast< std::ptrdiff_t >( indent ) } );
			}

			auto start = std::size_t{ 0 };
			auto first = true;

			while ( start < clusters.size( ) ) {
				auto line = first ? head : tail;
				auto width = prefix_width;
				auto cursor = start;
				auto break_at = start;

				while ( cursor < clusters.size( ) ) {
					const auto& cluster = clusters[ cursor ];

					if ( width + cluster.width > budget ) {
						break;
					}

					width += cluster.width;

					if ( is_space_cluster( cluster.text ) ) {
						break_at = cursor;
					}

					++cursor;
				}

				auto stop = cursor;

				if ( cursor == start ) {
					stop = start + 1;
				} else if ( cursor < clusters.size( ) && break_at > start ) {
					stop = break_at;
				}

				append_clusters( line, clusters, start, stop );
				rows.push_back( std::move( line ) );

				if ( stop >= clusters.size( ) ) {
					break;
				}

				start = stop;

				while ( start < clusters.size( ) && is_space_cluster( clusters[ start ].text ) ) {
					++start;
				}

				first = false;
			}

			return rows;
		}

	}

	namespace transcript {

		auto render_block( const std::string_view text, const token base )
			-> std::vector< styled_line > {
			if ( text.empty( ) ) {
				return { };
			}

			auto rows = render_markdown( text, base );

			while ( !rows.empty( ) && blank_row( rows.back( ) ) ) {
				rows.pop_back( );
			}

			return rows;
		}

		auto tail_rows( std::vector< styled_line > rows, const std::size_t max_rows )
			-> std::vector< styled_line > {
			if ( rows.size( ) <= max_rows ) {
				return rows;
			}

			rows.erase( rows.begin( ), rows.begin( ) +
				static_cast< std::ptrdiff_t >( rows.size( ) - max_rows ) );

			return rows;
		}

		auto prefix_row( const styled_line& row, const std::string_view gutter,
			const token color ) -> styled_line {
			auto out = styled_line{ };
			out.reserve( row.size( ) + 1 );
			out.push_back( { std::string{ gutter } + " ", color } );
			out.insert( out.end( ), row.begin( ), row.end( ) );

			return out;
		}

		auto wrap_row( const styled_line& row, const std::size_t columns,
			const std::size_t ambiguous_width ) -> std::vector< styled_line > {
			if ( columns == 0 || !needs_wrap( row, columns, ambiguous_width ) ) {
				return { row };
			}

			return split_row( row, columns, ambiguous_width );
		}

		auto wrap_rows( std::vector< styled_line > rows, const std::size_t columns,
			const std::size_t ambiguous_width ) -> std::vector< styled_line > {
			auto wrapped = std::vector< styled_line >{ };
			wrapped.reserve( rows.size( ) );

			for ( const auto& row : rows ) {
				for ( auto& split : wrap_row( row, columns, ambiguous_width ) ) {
					wrapped.push_back( std::move( split ) );
				}
			}

			return wrapped;
		}

	}

	auto render_coordinator::queue_block( std::vector< styled_line > lines ) -> void {
		if ( lines.empty( ) ) {
			return;
		}

		if ( transcript_started_ ) {
			state_.pending_commit.push_back( styled_line{ } );
		}

		queue_rows( std::move( lines ) );
	}

	auto render_coordinator::queue_rows( std::vector< styled_line > lines ) -> void {
		transcript_started_ = true;

		for ( auto& line : lines ) {
			state_.pending_commit.push_back( std::move( line ) );
		}
	}

	auto render_coordinator::queue_text( const std::string_view text, const token color ) -> void {
		auto body = text;

		while ( !body.empty( ) && ( body.back( ) == '\n' || body.back( ) == '\r' ) ) {
			body.remove_suffix( 1 );
		}

		if ( body.empty( ) ) {
			return;
		}

		auto rows = std::vector< styled_line >{ };
		auto start = std::size_t{ 0 };

		while ( start <= body.size( ) ) {
			const auto stop = body.find( '\n', start );
			const auto end = stop == std::string_view::npos ? body.size( ) : stop;
			auto line = body.substr( start, end - start );

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.remove_suffix( 1 );
			}

			rows.push_back( styled_line{ { std::string{ line }, color } } );

			if ( stop == std::string_view::npos ) {
				break;
			}

			start = stop + 1;
		}

		queue_block( std::move( rows ) );
	}

	auto render_coordinator::queue_thought( ) -> void {
		auto rows = transcript::tail_rows( state_.thinking_rows, THOUGHT_COMMIT_MAX_ROWS );

		if ( rows.empty( ) ) {
			return;
		}

		for ( auto& row : rows ) {
			row = transcript::prefix_row( row, GUTTER_THOUGHT, token::thinking );
		}

		queue_rows( std::move( rows ) );

		state_.thinking_text.clear( );
		state_.thinking_rows.clear( );
	}

	auto render_coordinator::commit( std::vector< styled_line > lines ) -> std::string {
		state_.pending_commit.clear( );

		lines = transcript::wrap_rows( std::move( lines ), screen_columns_,
			caps_.ambiguous_width );

		retain( lines );

		auto emitter = ansi_emitter{ caps_ };

		auto out = park();
		out += emitter.region_top( painted_rows_ );
		out += "\x1b[0J";

		for ( const auto& line : lines ) {
			auto pen = ansi_emitter{ caps_ };

			for ( const auto& span : line ) {
				out += pen.sgr( span_style( span ) );
				out += span.text;
			}

			// Raw mode's newline moves down without returning the carriage, so a
			// multi-row block would stair-step across the screen.
			out += "\x1b[0m\r\n";
		}

		for ( auto index = std::size_t{ 0 }; index < painted_rows_; ++index ) {
			out += '\n';
		}

		out += park();

		previous_.clear( );

		return out;
	}

}
