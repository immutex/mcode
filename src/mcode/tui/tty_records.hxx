#pragma once

#if defined( _WIN32 )

#include <vector>

#include <windows.h>

#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	// `ReadConsoleInputA` removes whatever it reports, so every record must be
	// consumed here or it is lost.
	auto absorb_records( const INPUT_RECORD* records, unsigned long count,
		decode_state& state, std::vector< key_event >& pending, bool mouse_reporting ) -> bool;

}

#endif
