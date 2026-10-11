#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t ROWS = 4;
	constexpr std::size_t COLUMNS = 40;

	[[nodiscard]] auto buffer_row_text( const cell_buffer& buffer, const std::size_t row )
		-> std::string {
		auto text = std::string{ };

		for ( auto column = std::size_t{ 0 }; column < buffer.columns( ); ++column ) {
			const auto& value = buffer.at( row, column );

			if ( value.width == 0 ) {
				continue;
			}

			text += value.text.empty( ) ? std::string{ " " } : value.text;
		}

		return text;
	}

	[[nodiscard]] auto buffer_contains( const cell_buffer& buffer,
		const std::string_view needle ) -> bool {
		for ( auto row = std::size_t{ 0 }; row < buffer.rows( ); ++row ) {
			if ( buffer_row_text( buffer, row ).find( needle ) != std::string::npos ) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] auto buffer_cell_column( const cell_buffer& buffer, const std::size_t row,
		const std::string_view needle ) -> std::size_t {
		const auto text = buffer_row_text( buffer, row );
		const auto at = text.find( needle );

		return at == std::string::npos ? buffer.columns( ) : at;
	}

}

TEST_CASE( "the status line shows the context percentage at its thresholds",
	"[tui][render]" ) {
	const auto status_row = ROWS - 2;

	const auto percent_style = [ & ]( const std::uint64_t used,
		const std::uint64_t capacity, const std::string_view needle ) -> style {
		auto state = render_state{ };
		state.model_name = "m";
		state.total_tokens = 1000;
		state.context_used = used;
		state.context_capacity = capacity;

		const auto frame = build_frame( state, ROWS, COLUMNS, 1 );
		const auto column = buffer_cell_column( frame, status_row, needle );

		REQUIRE( column < COLUMNS );

		return frame.at( status_row, column ).cell_style;
	};

	CHECK( percent_style( 50, 100, "50%" ).foreground == token::text );

	CHECK( percent_style( 80, 100, "80%" ).foreground == token::warn );

	CHECK( percent_style( 95, 100, "95%" ).foreground == token::error );

	// an over-full window reads as 100%, not as a value past the cap.
	auto over = render_state{ };
	over.model_name = "m";
	over.total_tokens = 1000;
	over.context_used = 200;
	over.context_capacity = 100;

	CHECK( buffer_contains( build_frame( over, ROWS, COLUMNS, 1 ), "100%" ) );
}

TEST_CASE( "an unknown capacity omits the context percentage", "[tui][render]" ) {
	auto state = render_state{ };
	state.model_name = "m";
	state.total_tokens = 1000;
	state.context_used = 500;
	state.context_capacity = 0;

	const auto frame = build_frame( state, ROWS, COLUMNS, 1 );

	CHECK( buffer_contains( frame, "1.0k tok" ) );
	CHECK_FALSE( buffer_contains( frame, "%" ) );
}

TEST_CASE( "the activity verb appears on the status line and clears",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	// wide enough that the verb and its hint are not clipped.
	coordinator.resize( 24, 80 );

	coordinator.set_meter( "m", 100, 0.0, 0 );
	(void)coordinator.flush( );

	coordinator.set_activity( "Working" );
	const auto busy = coordinator.flush( );

	CHECK( busy.find( "Working..." ) != std::string::npos );
	CHECK( busy.find( "esc to interrupt" ) != std::string::npos );

	coordinator.set_activity( "" );
	const auto idle = coordinator.flush( );

	CHECK( idle.find( "Working" ) == std::string::npos );
	CHECK( idle.find( "esc to interrupt" ) == std::string::npos );
}

TEST_CASE( "the activity verb rides the status row without moving the layout",
	"[tui][render]" ) {
	constexpr std::size_t WIDE_COLUMNS = 80;
	const auto status_row = ROWS - 2;

	auto busy = render_state{ };
	busy.model_name = "m";
	busy.total_tokens = 1000;
	busy.activity = "Running bash";

	auto idle = busy;
	idle.activity.clear( );

	const auto busy_frame = build_frame( busy, ROWS, WIDE_COLUMNS, 1 );
	const auto idle_frame = build_frame( idle, ROWS, WIDE_COLUMNS, 1 );

	// the verb is on the status row, and the prompt row is untouched.
	REQUIRE( buffer_contains( busy_frame, "Running bash..." ) );

	const auto verb_column = buffer_cell_column( busy_frame, status_row, "Running bash..." );

	CHECK( busy_frame.at( status_row, verb_column ).cell_style.foreground == token::accent );

	const auto hint_column = buffer_cell_column( busy_frame, status_row, "esc to interrupt" );

	REQUIRE( hint_column < WIDE_COLUMNS );
	CHECK( busy_frame.at( status_row, hint_column ).cell_style.dim );

	CHECK( buffer_row_text( busy_frame, ROWS - 1 ) ==
		buffer_row_text( idle_frame, ROWS - 1 ) );

	CHECK( region_rows_for( busy, 30 ) == region_rows_for( idle, 30 ) );
}
