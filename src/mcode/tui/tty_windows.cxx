#if defined( _WIN32 )

#include "mcode/tui/tty.hxx"

#include <array>
#include <chrono>
#include <string>
#include <utility>

#include <windows.h>

#include "mcode/platform/seams.hxx"
#include "mcode/tui/tty_records.hxx"

namespace mcode::tui {

	namespace {

		// Raw mode disables echo, so erasing a character is the session's job:
		// backspace, blank over the cell, backspace again.
		inline constexpr std::string_view ERASE_SEQUENCE = "\b \b";

		// No line editing, no echo, and window records so a resize is visible.
		inline constexpr unsigned long RAW_INPUT_MODE =
			ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;

		// Wheel and button records. NOT `ENABLE_VIRTUAL_TERMINAL_INPUT`: with
		// it set, conhost translates key input into VT bytes and
		// `ReadConsoleInputA` returns one character per record with a zero
		// virtual key, which erases every `VK_*` mapping. The wheel arrives as
		// a MOUSE_EVENT either way.
		inline constexpr unsigned long MOUSE_INPUT_MODE = ENABLE_MOUSE_INPUT;

		// How long `poll_resize` sleeps between checks of the record stream.
		inline constexpr unsigned long POLL_SLICE_MS = 10;

	}

	auto tty_session::create( ) -> result< tty_session > {
		auto session = tty_session{ };

		session.input_handle_ = GetStdHandle( STD_INPUT_HANDLE );
		session.output_handle_ = GetStdHandle( STD_OUTPUT_HANDLE );

		if ( session.input_handle_ == INVALID_HANDLE_VALUE ||
			session.output_handle_ == INVALID_HANDLE_VALUE ) {
			return std::unexpected( fail( errc::io, "no console handles" ) );
		}

		auto info = CONSOLE_SCREEN_BUFFER_INFO{ };

		if ( GetConsoleScreenBufferInfo( session.output_handle_, &info ) == 0 ) {
			return std::unexpected( fail( errc::io, "stdout is not a console" ) );
		}

		if ( GetConsoleMode( session.input_handle_, &session.in_mode_ ) == 0 ||
			GetConsoleMode( session.output_handle_, &session.out_mode_ ) == 0 ) {
			return std::unexpected( fail( errc::io, "GetConsoleMode failed" ) );
		}

		// `saved_` is set before the first mode change, so every exit path
		// restores through the destructor.
		session.saved_ = true;

		const auto raw_input = RAW_INPUT_MODE;
		const auto vt_output = session.out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

		if ( SetConsoleMode( session.input_handle_, raw_input ) == 0 ||
			SetConsoleMode( session.output_handle_, vt_output ) == 0 ) {
			return std::unexpected( fail( errc::io, "SetConsoleMode failed" ) );
		}

		// Every glyph a frame carries is UTF-8 -- the separators, the spinner,
		// the box drawing -- and the session writes them as raw bytes. The
		// console decodes those bytes with its own output codepage, so under
		// the default OEM page one separator arrives as three CP437 glyphs.
		//
		// Not fatal on failure: the codepage is an output cosmetic and a
		// redirected or hosted console can refuse it. A session with mojibake
		// beats no session.
		session.saved_output_cp_ = GetConsoleOutputCP( );

		if ( session.saved_output_cp_ != 0 ) {
			SetConsoleOutputCP( CP_UTF8 );
		}

		// The input side has the mirror problem: keystrokes and pastes are
		// decoded by the console with the INPUT codepage before the session
		// ever sees them, so under the legacy default a non-ASCII character
		// arrives as a byte sequence that is not valid UTF-8 and the renderer
		// paints the gap. Same tolerance as the output page.
		session.saved_input_cp_ = GetConsoleCP( );

		if ( session.saved_input_cp_ != 0 ) {
			SetConsoleCP( CP_UTF8 );
		}

		session.raw_active_ = true;
		session.attached_ = true;

		const auto has_tty = true;
		session.caps_ = probe_capabilities( tty_environment( "COLORTERM" ),
			tty_environment( "TERM" ), tty_environment( "NO_COLOR" ) == "1", has_tty,
			tty_environment( "MCODE_AMBIGUOUS_WIDTH" ) );

		// Wrapped here and unwrapped in `restore`, the same place the mouse
		// mode is handled, so the pair cannot drift apart. Windows Terminal
		// wraps a paste in the markers and reports them as key records, which
		// the record decoder turns back into one paste event.
		if ( session.caps_.bracketed_paste ) {
			session.write( BRACKETED_PASTE_ENABLE );
		}

		return session;
	}

	tty_session::tty_session( tty_session&& other ) noexcept
		: caps_( other.caps_ ), raw_active_( other.raw_active_ ),
		attached_( other.attached_ ), mouse_reporting_( other.mouse_reporting_ ),
		resize_pending_( other.resize_pending_ ), input_handle_( other.input_handle_ ),
		output_handle_( other.output_handle_ ), in_mode_( other.in_mode_ ),
		out_mode_( other.out_mode_ ), saved_output_cp_( other.saved_output_cp_ ),
		saved_input_cp_( other.saved_input_cp_ ), saved_( other.saved_ ) {
		other.attached_ = false;
		other.saved_ = false;
		other.saved_output_cp_ = 0;
		other.saved_input_cp_ = 0;
		other.raw_active_ = false;
		other.mouse_reporting_ = false;
		other.resize_pending_ = false;
	}

	auto tty_session::operator=( tty_session&& other ) noexcept -> tty_session& {
		if ( this != &other ) {
			restore( );

			caps_ = other.caps_;
			raw_active_ = other.raw_active_;
			attached_ = other.attached_;
			mouse_reporting_ = other.mouse_reporting_;
			resize_pending_ = other.resize_pending_;
			input_handle_ = other.input_handle_;
			output_handle_ = other.output_handle_;
			in_mode_ = other.in_mode_;
			out_mode_ = other.out_mode_;
			saved_output_cp_ = other.saved_output_cp_;
			saved_input_cp_ = other.saved_input_cp_;
			saved_ = other.saved_;

			other.attached_ = false;
			other.saved_ = false;
			other.saved_output_cp_ = 0;
			other.saved_input_cp_ = 0;
			other.raw_active_ = false;
			other.mouse_reporting_ = false;
			other.resize_pending_ = false;
		}

		return *this;
	}

	auto tty_session::restore( ) noexcept -> void {
		if ( !saved_ ) {
			return;
		}

		saved_ = false;
		raw_active_ = false;
		mouse_reporting_ = false;
		resize_pending_ = false;

		// The enable is written in `create`, so the disable belongs here: leaving the
		// terminal wrapped would leak the paste mode into whatever runs next.
		if ( caps_.bracketed_paste ) {
			write( BRACKETED_PASTE_DISABLE );
		}

		// The saved input mode predates raw mode, so it clears the mouse bits.
		SetConsoleMode( input_handle_, in_mode_ );
		SetConsoleMode( output_handle_, out_mode_ );

		if ( saved_output_cp_ != 0 ) {
			SetConsoleOutputCP( saved_output_cp_ );
		}

		if ( saved_input_cp_ != 0 ) {
			SetConsoleCP( saved_input_cp_ );
		}
	}

	tty_session::~tty_session( ) {
		restore( );
	}

	auto tty_session::write( const std::string_view bytes ) -> void {
		if ( !attached_ || bytes.empty( ) ) {
			return;
		}

		auto written = static_cast< unsigned long >( 0 );
		WriteFile( output_handle_, bytes.data( ),
			static_cast< unsigned long >( bytes.size( ) ), &written, nullptr );
		FlushFileBuffers( output_handle_ );
	}

	auto tty_session::read_line( const std::uint32_t wait_ms )
		-> std::optional< std::string > {
		if ( !attached_ ) {
			return std::nullopt;
		}

		auto line = std::string{ };
		auto state = decode_state{ };
		auto events = std::vector< key_event >{ };
		auto got_enter = false;

		const auto deadline = GetTickCount64( ) + wait_ms;

		while ( !got_enter ) {
			auto available = static_cast< unsigned long >( 0 );

			if ( GetNumberOfConsoleInputEvents( input_handle_, &available ) == 0 ) {
				return std::nullopt;
			}

			if ( available == 0 ) {
				if ( GetTickCount64( ) >= deadline ) {
					return std::nullopt;
				}

				Sleep( 10 );

				continue;
			}

			auto records = std::array< INPUT_RECORD, 32 >{ };
			auto count = static_cast< unsigned long >( 0 );

			if ( ReadConsoleInputA( input_handle_, records.data( ),
				static_cast< unsigned long >( records.size( ) ), &count ) == 0 ) {
				return std::nullopt;
			}

			// The same decoder the prompt uses, so a bracketed paste is
			// content here too and its markers never land in the answer.
			if ( absorb_records( records.data( ), count, state, events, false ) ) {
				resize_pending_ = true;
			}

			// Raw mode disabled echo, so nothing the user types reaches the
			// screen unless the session writes it back. Only accepted keys are
			// echoed, so a read that times out leaves the display untouched.
			for ( const auto& event : events ) {
				if ( event.type == key_event::kind::enter ) {
					// The caller advances its own row, so the key itself only
					// returns the cursor to column zero.
					write( "\r\n" );

					got_enter = true;

					break;
				}

				if ( event.type == key_event::kind::backspace ) {
					if ( !line.empty( ) ) {
						line.pop_back( );

						write( ERASE_SEQUENCE );
					}

					continue;
				}

				if ( event.type == key_event::kind::paste ) {
					// A pasted answer: its first line is the whole answer, the
					// way typing that line would have been.
					const auto stop = event.text.find( '\n' );
					const auto text = stop == std::string::npos ? event.text
						: event.text.substr( 0, stop );

					line += text;

					write( text );
					write( "\r\n" );

					got_enter = true;

					break;
				}

				if ( event.type == key_event::kind::character ) {
					line += event.text;

					write( event.text );
				}
			}

			events.clear( );
		}

		return line;
	}

	auto tty_session::size( ) const -> std::pair< int, int > {
		const auto measured = platform::terminal_size( );

		return measured ? *measured : std::pair{ 80, 24 };
	}

	auto tty_session::resized( ) const -> bool {
		return platform::terminal_size_changed( );
	}

	auto tty_session::set_mouse_reporting( const bool enabled ) -> void {
		if ( mouse_reporting_ == enabled ) {
			return;
		}

		mouse_reporting_ = enabled;

		if ( !attached_ ) {
			return;
		}

		// Toggling the wheel off restores the exact raw mode, so the keyboard
		// path is byte-for-byte what it was before.
		SetConsoleMode( input_handle_,
			enabled ? RAW_INPUT_MODE | MOUSE_INPUT_MODE : RAW_INPUT_MODE );
	}

	auto tty_session::poll_resize( const std::uint32_t wait_ms ) -> bool {
		const auto deadline = GetTickCount64( ) + wait_ms;

		while ( true ) {
			if ( resize_pending_ ) {
				resize_pending_ = false;

				return true;
			}

			// A platform that missed the record still notices the size diff.
			if ( platform::terminal_size_changed( ) ) {
				return true;
			}

			if ( !attached_ || GetTickCount64( ) >= deadline ) {
				return false;
			}

			auto available = static_cast< unsigned long >( 0 );

			if ( GetNumberOfConsoleInputEvents( input_handle_, &available ) != 0 &&
				available != 0 ) {
				auto records = std::array< INPUT_RECORD, 32 >{ };
				auto count = static_cast< unsigned long >( 0 );

				if ( ReadConsoleInputA( input_handle_, records.data( ),
					static_cast< unsigned long >( records.size( ) ), &count ) != 0 &&
					absorb_records( records.data( ), count, decode_, pending_,
						mouse_reporting_ ) ) {
					return true;
				}
			}

			Sleep( POLL_SLICE_MS );
		}
	}

	auto tty_session::read_key( const std::uint32_t wait_ms ) -> key_event {
		// Keys decoded from an earlier read that the caller has not asked for
		// yet. One console read returns several records, and this returns one
		// key, so the surplus waits here instead of being dropped.
		if ( !pending_.empty( ) ) {
			auto event = pending_.front( );
			pending_.erase( pending_.begin( ) );

			return event;
		}

		auto event = key_event{ };

		if ( !attached_ ) {
			event.type = key_event::kind::exit;

			return event;
		}

		const auto deadline = GetTickCount64( ) + wait_ms;

		while ( true ) {
			auto available = static_cast< unsigned long >( 0 );

			if ( GetNumberOfConsoleInputEvents( input_handle_, &available ) == 0 ) {
				event.type = key_event::kind::exit;

				return event;
			}

			if ( available == 0 ) {
				// An idle prompt polls; it does not end the session. Returning
				// `exit` here made a quiet terminal quit after the wait, which
				// the caller cannot distinguish from Ctrl+D.
				if ( GetTickCount64( ) >= deadline ) {
					// A marker prefix that never completed is ordinary input,
					// so a lone Escape is still the escape key. The bytes go
					// back through the keystroke decoder, which holds an
					// incomplete escape sequence in its own carry.
					if ( decode_.paste.holding( ) ) {
						decode_plain_key_bytes( decode_.paste.flush( ), decode_, pending_,
							mouse_reporting_ );
					}

					if ( !pending_.empty( ) ) {
						continue;
					}

					// an ESC held back for a sequence that never arrived is the key
					if ( decode_.carry == "\x1b" ) {
						decode_.carry.clear( );
						event.type = key_event::kind::escape;

						return event;
					}

					event.type = key_event::kind::timeout;

					return event;
				}

				Sleep( 10 );

				continue;
			}

			auto records = std::array< INPUT_RECORD, 32 >{ };
			auto count = static_cast< unsigned long >( 0 );

			if ( ReadConsoleInputA( input_handle_, records.data( ),
				static_cast< unsigned long >( records.size( ) ), &count ) == 0 ) {
				event.type = key_event::kind::exit;

				return event;
			}

			// `ReadConsoleInputA` REMOVES every record it reports, so anything
			// decoded here and not returned is lost. The whole batch is decoded
			// into `pending_` and the first key is returned, which is what
			// stopped fast typing from dropping characters.
			if ( absorb_records( records.data( ), count, decode_, pending_,
				mouse_reporting_ ) ) {
				resize_pending_ = true;
			}

			if ( pending_.empty( ) ) {
				// Only releases, mouse or resize records: not a key.
				if ( GetTickCount64( ) >= deadline ) {
					event.type = key_event::kind::timeout;

					return event;
				}

				continue;
			}

			auto first = pending_.front( );
			pending_.erase( pending_.begin( ) );

			return first;
		}
	}
}

#endif
