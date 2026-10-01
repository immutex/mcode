#include "mcode/tui/tty.hxx"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

#include "mcode/platform/seams.hxx"

namespace mcode::tui {

	auto probe_capabilities( const std::string_view colorterm, const std::string_view term,
		const bool no_color, const bool has_tty ) -> capabilities {
		auto out = capabilities{ };
		out.is_tty = has_tty;

		// NO_COLOR wins: attributes only, no foreground or background changes.
		if ( no_color ) {
			out.depth = capabilities::color_depth::none;
		} else if ( colorterm.find( "truecolor" ) != std::string_view::npos ||
			colorterm.find( "24bit" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::truecolor;
		} else if ( term.find( "256color" ) != std::string_view::npos ||
			colorterm.find( "256color" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::ansi256;
		} else if ( term.find( "xterm" ) != std::string_view::npos ||
			term.find( "screen" ) != std::string_view::npos ||
			term.find( "tmux" ) != std::string_view::npos ||
			term.find( "vt100" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::ansi256;
		} else if ( has_tty ) {
			out.depth = capabilities::color_depth::ansi16;
		} else {
			out.depth = capabilities::color_depth::none;
		}

		// The synchronized-output and Kitty probes write to the terminal, so
		// they are gated on a real tty. DECRQM is the gate, never TERM text.
		out.synchronized_output = has_tty;
		out.bracketed_paste = has_tty;
		out.kitty_keyboard = has_tty;

		return out;
	}

	namespace {

		[[nodiscard]] auto environment( const char* name ) -> std::string_view {
			const auto* value = std::getenv( name );

			return value != nullptr ? std::string_view{ value } : std::string_view{ };
		}

	}

#if defined( _WIN32 )

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

		// Once the saved modes are captured, saved_ is true, so every exit
		// path -- including a failed SetConsoleMode below -- restores through
		// the destructor. Restoring a mode that was never changed is
		// harmless: the restore writes back the values that were read.
		session.saved_ = true;

		// Raw input: no line discipline, no echo, no signal characters. The
		// editor owns all of it.
		const auto raw_input = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
		const auto vt_output = session.out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

		if ( SetConsoleMode( session.input_handle_, raw_input ) == 0 ||
			SetConsoleMode( session.output_handle_, vt_output ) == 0 ) {
			return std::unexpected( fail( errc::io, "SetConsoleMode failed" ) );
		}

		session.raw_active_ = true;
		session.attached_ = true;

		const auto has_tty = true;
		session.caps_ = probe_capabilities( environment( "COLORTERM" ),
			environment( "TERM" ), environment( "NO_COLOR" ) == "1", has_tty );

		return session;
	}

	tty_session::tty_session( tty_session&& other ) noexcept
		: caps_( other.caps_ ), raw_active_( other.raw_active_ ),
		attached_( other.attached_ ), input_handle_( other.input_handle_ ),
		output_handle_( other.output_handle_ ), in_mode_( other.in_mode_ ),
		out_mode_( other.out_mode_ ), saved_( other.saved_ ) {
		other.attached_ = false;
		other.saved_ = false;
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
			saved_ = other.saved_;

			other.attached_ = false;
			other.saved_ = false;
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

		// Best effort on the way out: a failed restore is worse than a silent
		// one, but a destructor cannot report it usefully either.
		SetConsoleMode( input_handle_, in_mode_ );
		SetConsoleMode( output_handle_, out_mode_ );
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
					got_enter = true;

					break;
				}

				if ( key.uChar.AsciiChar == 0 ) {
					continue;
				}

				line.push_back( key.uChar.AsciiChar );
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

	auto tty_session::read_key( const std::uint32_t wait_ms ) -> key_event {
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

			auto records = std::array< INPUT_RECORD, 8 >{ };
			auto count = static_cast< unsigned long >( 0 );

			if ( ReadConsoleInputA( input_handle_, records.data( ),
				static_cast< unsigned long >( records.size( ) ), &count ) == 0 ) {
				event.type = key_event::kind::exit;

				return event;
			}

			for ( auto index = static_cast< unsigned long >( 0 ); index < count; ++index ) {
				const auto& record = records[ index ];

				if ( record.EventType != KEY_EVENT ||
					record.Event.KeyEvent.bKeyDown == 0 ) {
					continue;
				}

				const auto& key = record.Event.KeyEvent;

				switch ( key.wVirtualKeyCode ) {
					case VK_RETURN: event.type = key_event::kind::enter; return event;
					case VK_BACK: event.type = key_event::kind::backspace; return event;
					case VK_DELETE: event.type = key_event::kind::delete_key; return event;
					case VK_LEFT: event.type = key_event::kind::left; return event;
					case VK_RIGHT: event.type = key_event::kind::right; return event;
					case VK_UP: event.type = key_event::kind::up; return event;
					case VK_DOWN: event.type = key_event::kind::down; return event;
					case VK_HOME: event.type = key_event::kind::home; return event;
					case VK_END: event.type = key_event::kind::end; return event;
					case VK_ESCAPE: event.type = key_event::kind::exit; return event;
					default: break;
				}

				const auto character = key.uChar.AsciiChar;

				if ( character == '\x03' ) {
					event.type = key_event::kind::interrupt;

					return event;
				}

				if ( character == '\x04' ) {
					event.type = key_event::kind::exit;

					return event;
				}

				if ( character == 0 ) {
					continue;
				}

				event.type = key_event::kind::character;
				event.text.assign( 1, character );

				return event;
			}
		}
	}


#else

	auto tty_session::create( ) -> result< tty_session > {
		auto session = tty_session{ };

		if ( ::isatty( STDIN_FILENO ) == 0 || ::isatty( STDOUT_FILENO ) == 0 ) {
			return std::unexpected( fail( errc::io, "stdin or stdout is not a terminal" ) );
		}

		if ( ::tcgetattr( STDIN_FILENO, &session.saved_termios_ ) != 0 ) {
			return std::unexpected( fail( errc::io, "tcgetattr failed" ) );
		}

		auto raw = session.saved_termios_;

		// cfmakeraw's effect, spelled out: no echo, no canonical line
		// editing, no signals from the keyboard.
		raw.c_lflag &= static_cast< unsigned int >( ~( ECHO | ICANON | ISIG | IEXTEN ) );
		raw.c_iflag &= static_cast< unsigned int >( ~( IXON | ICRNL | BRKINT | INPCK | ISTRIP ) );
		raw.c_oflag &= static_cast< unsigned int >( ~OPOST );
		raw.c_cflag |= CS8;
		raw.c_cc[ VMIN ] = 0;
		raw.c_cc[ VTIME ] = 1;

		if ( ::tcsetattr( STDIN_FILENO, TCSANOW, &raw ) != 0 ) {
			return std::unexpected( fail( errc::io, "tcsetattr failed" ) );
		}

		session.saved_ = true;
		session.raw_active_ = true;
		session.attached_ = true;

		const auto has_tty = true;
		session.caps_ = probe_capabilities( environment( "COLORTERM" ),
			environment( "TERM" ), environment( "NO_COLOR" ) == "1", has_tty );

		return session;
	}

	tty_session::tty_session( tty_session&& other ) noexcept
		: caps_( other.caps_ ), raw_active_( other.raw_active_ ),
		attached_( other.attached_ ), saved_( other.saved_ ),
		saved_termios_( other.saved_termios_ ) {
		other.attached_ = false;
		other.saved_ = false;
		other.raw_active_ = false;
	}

	auto tty_session::operator=( tty_session&& other ) noexcept -> tty_session& {
		if ( this != &other ) {
			restore( );

			caps_ = other.caps_;
			raw_active_ = other.raw_active_;
			attached_ = other.attached_;
			saved_ = other.saved_;
			saved_termios_ = other.saved_termios_;

			other.attached_ = false;
			other.saved_ = false;
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

		::tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios_ );
	}

	tty_session::~tty_session( ) {
		restore( );
	}

	auto tty_session::write( const std::string_view bytes ) -> void {
		if ( !attached_ || bytes.empty( ) ) {
			return;
		}

		auto offset = std::size_t{ 0 };

		while ( offset < bytes.size( ) ) {
			const auto written = ::write( STDOUT_FILENO, bytes.data( ) + offset,
				bytes.size( ) - offset );

			if ( written <= 0 ) {
				break;
			}

			offset += static_cast< std::size_t >( written );
		}
	}

	auto tty_session::read_line( const std::uint32_t wait_ms )
		-> std::optional< std::string > {
		if ( !attached_ ) {
			return std::nullopt;
		}

		auto line = std::string{ };
		auto got_enter = false;
		auto got_eof = false;

		while ( !got_enter && !got_eof ) {
			auto read_set = fd_set{ };
			FD_ZERO( &read_set );
			FD_SET( STDIN_FILENO, &read_set );

			// Cast to each member's own type: `tv_sec` is `time_t` and
			// `tv_usec` is `suseconds_t`, which is `long` on Linux and `int`
			// on Darwin, so naming either one explicitly is wrong on the other.
			auto timeout = timeval{ };
			timeout.tv_sec = static_cast< decltype( timeout.tv_sec ) >( wait_ms / 1000 );
			timeout.tv_usec = static_cast< decltype( timeout.tv_usec ) >(
				( wait_ms % 1000 ) * 1000 );

			const auto ready = ::select( STDIN_FILENO + 1, &read_set, nullptr, nullptr,
				&timeout );

			if ( ready <= 0 ) {
				return std::nullopt;
			}

			auto byte = char{ };
			const auto count = ::read( STDIN_FILENO, &byte, 1 );

			if ( count <= 0 ) {
				got_eof = true;

				break;
			}

			if ( byte == '\r' || byte == '\n' ) {
				got_enter = true;

				break;
			}

			line.push_back( byte );
		}

		if ( got_eof && line.empty( ) ) {
			return std::nullopt;
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

	auto tty_session::read_key( const std::uint32_t wait_ms ) -> key_event {
		auto event = key_event{ };

		if ( !attached_ ) {
			event.type = key_event::kind::exit;

			return event;
		}

		auto read_set = fd_set{ };
		FD_ZERO( &read_set );
		FD_SET( STDIN_FILENO, &read_set );

		auto timeout = timeval{ };
		timeout.tv_sec = static_cast< decltype( timeout.tv_sec ) >( wait_ms / 1000 );
		timeout.tv_usec = static_cast< decltype( timeout.tv_usec ) >(
			( wait_ms % 1000 ) * 1000 );

		const auto ready = ::select( STDIN_FILENO + 1, &read_set, nullptr, nullptr,
			&timeout );

		if ( ready == 0 ) {
			// The wait elapsed. Idle is not exit: the caller polls.
			event.type = key_event::kind::timeout;

			return event;
		}

		if ( ready < 0 ) {
			event.type = key_event::kind::exit;

			return event;
		}

		auto bytes = std::array< char, 8 >{ };
		const auto count = ::read( STDIN_FILENO, bytes.data( ), bytes.size( ) );

		if ( count <= 0 ) {
			event.type = key_event::kind::exit;

			return event;
		}

		const auto text = std::string_view{ bytes.data( ),
			static_cast< std::size_t >( count ) };

		if ( text == "\x1b[A" ) { event.type = key_event::kind::up; return event; }
		if ( text == "\x1b[B" ) { event.type = key_event::kind::down; return event; }
		if ( text == "\x1b[C" ) { event.type = key_event::kind::right; return event; }
		if ( text == "\x1b[D" ) { event.type = key_event::kind::left; return event; }
		if ( text == "\x1b[H" || text == "\x1b[1~" ) {
			event.type = key_event::kind::home;

			return event;
		}
		if ( text == "\x1b[F" || text == "\x1b[4~" ) {
			event.type = key_event::kind::end;

			return event;
		}

		const auto first = text.front( );

		if ( first == '\r' || first == '\n' ) {
			event.type = key_event::kind::enter;

			return event;
		}

		if ( first == '\x7F' || first == '\x08' ) {
			event.type = key_event::kind::backspace;

			return event;
		}

		if ( first == '\x03' ) {
			event.type = key_event::kind::interrupt;

			return event;
		}

		if ( first == '\x04' ) {
			event.type = key_event::kind::exit;

			return event;
		}

		if ( static_cast< unsigned char >( first ) < 0x20 ) {
			event.type = key_event::kind::exit;

			return event;
		}

		event.type = key_event::kind::character;
		event.text.assign( text.data( ), text.size( ) );

		return event;
	}


#endif

	auto input_decoder::feed( const std::string_view bytes ) -> std::vector< std::string > {
		auto out = std::vector< std::string >{ };

		for ( const auto character : bytes ) {
			if ( closed_ ) {
				break;
			}

			if ( character == 0x04 ) {
				// Ctrl+D on an empty line is end-of-input; on a partial line
				// it is nothing, matching every shell.
				if ( pending_.empty( ) ) {
					closed_ = true;
				}

				continue;
			}

			if ( character == '\r' || character == '\n' ) {
				out.push_back( pending_ );
				pending_.clear( );

				continue;
			}

			if ( character == 0x7F || character == 0x08 ) {
				// Backspace deletes one byte, which is one ASCII character.
				// Multi-byte clusters are the editor's problem; the decoder
				// only guarantees line framing.
				if ( !pending_.empty( ) ) {
					pending_.pop_back( );
				}

				continue;
			}

			if ( static_cast< unsigned char >( character ) < 0x20 && character != '\t' ) {
				continue;
			}

			pending_.push_back( character );
		}

		return out;
	}

}
