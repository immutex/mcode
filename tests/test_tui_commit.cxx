#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/transcript.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	[[nodiscard]] auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	[[nodiscard]] auto row_bold( const styled_line& row ) -> bool {
		for ( const auto& span : row ) {
			if ( span.bold ) {
				return true;
			}
		}

		return false;
	}

	// a committed row carries its own SGR, so its text is read with the escapes removed
	[[nodiscard]] auto visible_text( const std::string_view bytes ) -> std::string {
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

}

TEST_CASE( "a committed answer is erased from the region and printed",
	"[tui][render]" ) {
	// the region must be erased before printing, or the next frame repaints over the text.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 100 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "Red";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "\x1b[0J" ) != std::string::npos );
	CHECK( bytes.find( "Red" ) != std::string::npos );
}

TEST_CASE( "a chain of thought commits as a block, not one line", "[tui][render]" ) {
	// the collapsed block shows the reasoning, bounded: the first line alone
	// hid everything the model actually thought.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = "first thought\nsecond thought\nthird thought";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first thought" ) != std::string::npos );
	CHECK( bytes.find( "second thought" ) != std::string::npos );
	CHECK( bytes.find( "third thought" ) != std::string::npos );
}

TEST_CASE( "a chain of thought longer than its budget commits its newest rows",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < THOUGHT_COMMIT_MAX_ROWS + 2; ++index ) {
		text += "thought " + std::to_string( index ) + "\n";
	}

	text += "last thought";

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = text;
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	// one committed row per rendered row, bounded to the budget.
	REQUIRE( coordinator.state( ).pending_commit.size( ) == THOUGHT_COMMIT_MAX_ROWS );

	const auto bytes = coordinator.flush( );

	// the block keeps its newest rows, so the head of the chain is dropped.
	CHECK( bytes.find( "✻ thought 3" ) != std::string::npos );
	CHECK( bytes.find( "last thought" ) != std::string::npos );
	CHECK( bytes.find( "thought 0" ) == std::string::npos );
}

TEST_CASE( "a turn's answer survives the turn ending", "[tui][render]" ) {
	// turn_end clears the live region, so the answer must be committed before it.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "the answer";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "the answer" ) != std::string::npos );
}

TEST_CASE( "the commit path wraps rows to the coordinator's own width",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	// one column stays reserved, so the commit wraps to 39 columns.
	coordinator.resize( 24, 40 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < 30; ++index ) {
		text += "token ";
	}

	coordinator.queue_text( text );

	const auto bytes = coordinator.flush( );

	auto start = bytes.find( "\x1b[0J" );

	REQUIRE( start != std::string::npos );
	start += 4;

	// the committed rows run between the erase and the region's scroll, each ending in a CRLF
	auto rows = std::vector< std::string >{ };

	while ( start < bytes.size( ) ) {
		const auto stop = bytes.find( "\r\n", start );

		if ( stop == std::string::npos ) {
			break;
		}

		rows.push_back( bytes.substr( start, stop - start ) );
		start = stop + 2;
	}

	REQUIRE( rows.size( ) > 1 );

	for ( const auto& row : rows ) {
		CHECK( string_width( visible_text( row ), 1 ) <= 39 );
	}
}

TEST_CASE( "a turn end commits the buffer, not the row cache", "[tui][render]" ) {
	// the deltas left the rows stale and no paint ran between them and the
	// end of the turn, so the commit is the only thing that materialises the
	// block. Committing the stale cache would print nothing at all.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "**bold** answer\nsecond row\n";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	// two rows, formatted, and no markdown markers in the committed block.
	REQUIRE( coordinator.state( ).pending_commit.size( ) == 2 );
	CHECK( row_text( coordinator.state( ).pending_commit[ 0 ] ) == "bold answer" );
	CHECK( row_bold( coordinator.state( ).pending_commit[ 0 ] ) );
	CHECK( row_text( coordinator.state( ).pending_commit[ 1 ] ) == "second row" );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "**" ) == std::string::npos );
	CHECK( visible_text( bytes ).find( "bold answer" ) != std::string::npos );

	// emphasis survives at depth `none`: the emitter drops only colour, never attributes
	CHECK( bytes.find( ";1m" ) != std::string::npos );
}
