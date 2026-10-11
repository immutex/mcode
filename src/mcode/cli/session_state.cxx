#include "mcode/cli/session_state.hxx"

#include <string>

namespace mcode::cli {

	auto session_state_provider( const mcode::event_log& log,
		const mcode::session_budget& budget )
		-> std::function< mcode::ext::session_state( ) > {
		return [ &log, &budget ]( ) {
			auto state = mcode::ext::session_state{ };

			if ( log.is_open( ) ) {
				state.session = log.path( ).stem( ).string( );
				state.seq = log.next_sequence( );
			}

			state.steps = budget.steps_used.load( );

			return state;
		};
	}

}
