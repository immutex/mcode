#include "mcode/tui/opening.hxx"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcode::tui {

	auto emit_opening( render_coordinator& coordinator, const session_opening& value )
		-> std::string {
		auto banner = std::vector< styled_line >{ };

		const auto banner_line = [ & ]( const std::string_view text, const token color ) {
			auto line = styled_line{ };
			line.push_back( { std::string{ text }, color, token::none, false, false, false } );
			banner.push_back( std::move( line ) );
		};

		banner_line( "", token::none );
		banner_line( "   mcode " + std::string{ value.version }, token::accent );
		banner_line( "   " + std::string{ value.platform } + ", "
			+ std::string{ value.compiler }, token::muted );
		banner_line( "", token::none );

		if ( value.permissive ) {
			banner_line( "   edits and commands run without prompting", token::warn );
			banner_line( "   the hard-deny floor and permissions.deny still apply", token::muted );
		}

		banner_line( "", token::none );
		banner_line( "   /help for commands, @ to mention a file, Ctrl+C to interrupt",
			token::muted );
		banner_line( "", token::none );

		coordinator.queue_block( std::move( banner ) );

		// One flush, no separate scroll: flush paints the region below the
		// banner, so a later scroll pushes the banner off the top of the screen.
		return coordinator.flush( );
	}

}
