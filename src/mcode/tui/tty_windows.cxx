
#if defined( _WIN32 )

#include "mcode/tui/tty.hxx"

#include <array>
#include <chrono>
#include <string>
#include <utility>

#include <windows.h>

#include "mcode/platform/seams.hxx"

namespace mcode::tui {

	namespace {

		// Raw mode disables echo, so erasing a character is the session's job:
		// backspace, blank over the cell, backspace again.
		inline constexpr std::string_view ERASE_SEQUENCE = "\b \b";

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

		const auto raw_input = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
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
			tty_environment( "TERM" ), tty_environment( "NO_COLOR" ) == "1", has_tty );

		return session;
	}

	tty_session::tty_session( tty_session&& other ) noexcept
		: caps_( other.caps_ ), raw_active_( other.raw_active_ ),
		attached_( other.attached_ ), input_handle_( other.input_handle_ ),
		output_handle_( other.output_handle_ ), in_mode_( other.in_mode_ ),
		out_mode_( other.out_mode_ ), saved_output_cp_( other.saved_output_cp_ ),
		saved_input_cp_( other.saved_input_cp_ ), saved_( other.saved_ ) {
		other.attached_ = false;
		other.saved_ = false;
		other.saved_output_cp_ = 0;
		other.saved_input_cp_ = 0;
		other.raw_active_ = false;
	}

	auto tty_session::operator=( tty_session&& other ) noexcept -> tty_session& {
		if ( this != &other ) {
			restore( );

			caps_ = other.caps_;
			raw_active_ = other.raw_active_;
			attached_ = other.attached_;
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
		}

		return *this;
	}

	auto tty_session::restore( ) noexcept -> void {
		if ( !saved_ ) {
			return;
		}

		saved_ = false;
		raw_active_ = false;

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
		auto got_enter = false;

		// Raw mode disabled echo, so nothing the user types reaches the screen
		// unless the session writes it back. Only accepted keys are echoed, so
		// a read that times out leaves the display untouched.
		const auto echo = [ this ]( const std::string_view bytes ) {
			auto written = static_cast< unsigned long >( 0 );

			WriteFile( output_handle_, bytes.data( ),
				static_cast< unsigned long >( bytes.size( ) ), &written, nullptr );
		};

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

			for ( auto index = static_cast< unsigned long >( 0 ); index < count; ++index ) {
				const auto& record = records[ index ];

				if ( record.EventType != KEY_EVENT ) {
					continue;
				}

				const auto& key = record.Event.KeyEvent;

				if ( key.bKeyDown == 0 ) {
					continue;
				}

				if ( key.uChar.AsciiChar == '\r' || key.uChar.AsciiChar == '\n' ) {
					// The caller advances its own row, so the key itself only
					// returns the cursor to column zero.
					echo( "\r\n" );

					got_enter = true;

					break;
				}

				if ( key.uChar.AsciiChar == 0 ) {
					continue;
				}

				if ( key.uChar.AsciiChar == '\b' ) {
					if ( !line.empty( ) ) {
						line.pop_back( );

						echo( ERASE_SEQUENCE );
					}

					continue;
				}

				line.push_back( key.uChar.AsciiChar );

				echo( std::string_view{ &key.uChar.AsciiChar, 1 } );
			}
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

	namespace {

		// A record carries `wRepeatCount`: ONE record standing for N presses
		// of a held key.
		auto decode_windows_key( const KEY_EVENT_RECORD& key ) -> std::vector< key_event > {
			auto out = std::vector< key_event >{ };
			auto event = key_event{ };

			switch ( key.wVirtualKeyCode ) {
				case VK_RETURN: event.type = key_event::kind::enter; break;
				case VK_TAB: event.type = key_event::kind::tab; break;
				case VK_BACK: event.type = key_event::kind::backspace; break;
				case VK_DELETE: event.type = key_event::kind::delete_key; break;
				case VK_LEFT: event.type = key_event::kind::left; break;
				case VK_RIGHT: event.type = key_event::kind::right; break;
				case VK_UP: event.type = key_event::kind::up; break;
				case VK_DOWN: event.type = key_event::kind::down; break;
				case VK_HOME: event.type = key_event::kind::home; break;
				case VK_END: event.type = key_event::kind::end; break;
				case VK_ESCAPE: event.type = key_event::kind::escape; break;
				default: break;
			}

			if ( event.type != key_event::kind::character ) {
				out.push_back( event );

				return out;
			}

			const auto character = key.uChar.AsciiChar;

			// Ctrl+C and Ctrl+D first: some hosts report them with a zero
			// `AsciiChar` and only the virtual key, so the character test alone
			// dropped them.
			if ( character == '\x03' ||
				( character == 0 && key.wVirtualKeyCode == 'C' &&
					( key.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED ) ) != 0 ) ) {
				event.type = key_event::kind::interrupt;

				out.push_back( event );

				return out;
			}

			if ( character == '\x04' ||
				( character == 0 && key.wVirtualKeyCode == 'D' &&
					( key.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED ) ) != 0 ) ) {
				event.type = key_event::kind::exit;

				out.push_back( event );

				return out;
			}

			if ( character == 0 ) {
				return out;
			}

			event.type = key_event::kind::character;
			event.text.assign( 1, character );

			// A held key reports its repeats in one record.
			const auto repeats = key.wRepeatCount == 0 ? 1u : key.wRepeatCount;

			for ( auto index = static_cast< unsigned int >( 0 ); index < repeats; ++index ) {
				out.push_back( event );
			}

			return out;
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
			for ( auto index = static_cast< unsigned long >( 0 ); index < count; ++index ) {
				const auto& record = records[ index ];

				if ( record.EventType != KEY_EVENT ||
					record.Event.KeyEvent.bKeyDown == 0 ) {
					continue;
				}

				for ( auto& decoded : decode_windows_key( record.Event.KeyEvent ) ) {
					pending_.push_back( std::move( decoded ) );
				}
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
