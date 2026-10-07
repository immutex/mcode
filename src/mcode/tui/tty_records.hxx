#pragma once

#if defined( _WIN32 )

#include <vector>

#include <windows.h>

#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	// Decodes one console batch into `pending` and reports whether the batch
	// carried a resize record. `ReadConsoleInputA` removes whatever it reports,
	// so every record must be consumed here or it is lost. A bracketed paste
	// arrives as records and is turned into one paste event by the shared
	// assembler in `decode_state`.
	auto absorb_records( const INPUT_RECORD* records, unsigned long count,
		decode_state& state, std::vector< key_event >& pending, bool mouse_reporting ) -> bool;

}

#endif
