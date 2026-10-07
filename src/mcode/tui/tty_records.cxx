// The Windows console record decoder. A record is translated into the bytes a
// terminal would have sent and handed to the shared decoder, so there is one
// implementation of what a key is and one of what a paste is; only the record
// fields themselves are read here.
#include "mcode/tui/tty_records.hxx"

#include <cstddef>

namespace mcode::tui {

	namespace {

		// One wheel notch; a record can carry several at once.
		inline constexpr int WHEEL_NOTCH = 120;

		// The wheel delta rides in the high word of `dwButtonState`.
		inline constexpr unsigned long WHEEL_DELTA_SHIFT = 16;
		inline constexpr unsigned long WHEEL_DELTA_MASK = 0xFFFF;

		// SGR wheel reporting, the encoding `set_mouse_reporting` asks for.
		// The coordinates are decoded and dropped, so they are zero here.
		inline constexpr std::string_view SGR_WHEEL_UP = "\x1b[<64;0;0M";
		inline constexpr std::string_view SGR_WHEEL_DOWN = "\x1b[<65;0;0M";

		// A record carries `wRepeatCount`: ONE record standing for N presses
		// of a held key.
		auto decode_windows_key( const KEY_EVENT_RECORD& key, decode_state& state,
			std::vector< key_event >& pending ) -> void {
			switch ( key.wVirtualKeyCode ) {
				case VK_RETURN:
					decode_key_bytes( "\r", state, pending, false );

					return;
				case VK_TAB:
					decode_key_bytes( "\t", state, pending, false );

					return;
				case VK_BACK:
					decode_key_bytes( "\x7F", state, pending, false );

					return;
				case VK_DELETE:
					decode_key_bytes( "\x1b[3~", state, pending, false );

					return;
				case VK_LEFT:
					decode_key_bytes( "\x1b[D", state, pending, false );

					return;
				case VK_RIGHT:
					decode_key_bytes( "\x1b[C", state, pending, false );

					return;
				case VK_UP:
					decode_key_bytes( "\x1b[A", state, pending, false );

					return;
				case VK_DOWN:
					decode_key_bytes( "\x1b[B", state, pending, false );

					return;
				case VK_HOME:
					decode_key_bytes( "\x1b[H", state, pending, false );

					return;
				case VK_END:
					decode_key_bytes( "\x1b[F", state, pending, false );

					return;
				case VK_PRIOR:
					decode_key_bytes( "\x1b[5~", state, pending, false );

					return;
				case VK_NEXT:
					decode_key_bytes( "\x1b[6~", state, pending, false );

					return;
				case VK_ESCAPE:
					// A bracketed paste's marker starts with this, so it goes
					// through the decoder rather than straight to the caller.
					decode_key_bytes( "\x1b", state, pending, false );

					return;
				default: break;
			}

			const auto character = key.uChar.AsciiChar;

			// Ctrl+C and Ctrl+D first: some hosts report them with a zero
			// `AsciiChar` and only the virtual key, so the character test alone
			// dropped them.
			if ( character == '\x03' ||
				( character == 0 && key.wVirtualKeyCode == 'C' &&
					( key.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED ) ) != 0 ) ) {
				decode_key_bytes( "\x03", state, pending, false );

				return;
			}

			if ( character == '\x04' ||
				( character == 0 && key.wVirtualKeyCode == 'D' &&
					( key.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED ) ) != 0 ) ) {
				decode_key_bytes( "\x04", state, pending, false );

				return;
			}

			if ( character == '\x12' ||
				( character == 0 && key.wVirtualKeyCode == 'R' &&
					( key.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED ) ) != 0 ) ) {
				decode_key_bytes( "\x12", state, pending, false );

				return;
			}

			if ( character == 0 ) {
				return;
			}

			// A held key reports its repeats in one record.
			const auto repeats = key.wRepeatCount == 0 ? 1u : key.wRepeatCount;

			for ( auto index = static_cast< unsigned int >( 0 ); index < repeats; ++index ) {
				decode_key_bytes( std::string_view{ &key.uChar.AsciiChar, 1 }, state,
					pending, false );
			}
		}

		// Wheel only: the button and position fields are decoded and dropped.
		auto decode_windows_mouse( const MOUSE_EVENT_RECORD& mouse, decode_state& state,
			std::vector< key_event >& pending, const bool mouse_reporting ) -> void {
			if ( ( mouse.dwEventFlags & MOUSE_WHEELED ) == 0 ) {
				return;
			}

			const auto delta = static_cast< short >(
				( mouse.dwButtonState >> WHEEL_DELTA_SHIFT ) & WHEEL_DELTA_MASK );

			if ( delta == 0 ) {
				return;
			}

			const auto magnitude = delta > 0 ? static_cast< int >( delta )
				: -static_cast< int >( delta );
			const auto notches = magnitude / WHEEL_NOTCH;
			const auto count = notches == 0 ? 1u : static_cast< unsigned int >( notches );
			const auto sequence = delta > 0 ? SGR_WHEEL_UP : SGR_WHEEL_DOWN;

			for ( auto index = static_cast< unsigned int >( 0 ); index < count; ++index ) {
				decode_key_bytes( sequence, state, pending, mouse_reporting );
			}
		}

	}

	auto absorb_records( const INPUT_RECORD* records, const unsigned long count,
		decode_state& state, std::vector< key_event >& pending,
		const bool mouse_reporting ) -> bool {
		auto saw_resize = false;

		for ( auto index = static_cast< unsigned long >( 0 ); index < count; ++index ) {
			const auto& record = records[ index ];

			if ( record.EventType == WINDOW_BUFFER_SIZE_EVENT ) {
				saw_resize = true;

				continue;
			}

			if ( record.EventType == MOUSE_EVENT ) {
				if ( !mouse_reporting ) {
					continue;
				}

				decode_windows_mouse( record.Event.MouseEvent, state, pending,
					mouse_reporting );

				continue;
			}

			if ( record.EventType != KEY_EVENT ||
				record.Event.KeyEvent.bKeyDown == 0 ) {
				continue;
			}

			decode_windows_key( record.Event.KeyEvent, state, pending );
		}

		return saw_resize;
	}

}
