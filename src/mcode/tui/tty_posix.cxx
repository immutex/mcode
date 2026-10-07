#if !defined( _WIN32 )

#include "mcode/tui/tty.hxx"

#include <array>
#include <cerrno>
#include <csignal>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "mcode/platform/seams.hxx"

namespace mcode::tui {

	namespace {

		// Raw mode disables echo, so erasing a character is the session's job:
		// backspace, blank over the cell, backspace again.
		inline constexpr std::string_view ERASE_SEQUENCE = "\b \b";

		// Wheel only: 1000 is the report mode, 1006 the SGR encoding.
		inline constexpr std::string_view MOUSE_ENABLE_SEQUENCE =
			"\x1b[?1000h\x1b[?1006h";
		inline constexpr std::string_view MOUSE_DISABLE_SEQUENCE =
			"\x1b[?1006l\x1b[?1000l";

		// How long `poll_resize` waits between checks of the flag.
		inline constexpr int RESIZE_POLL_SLICE_MS = 10;

		// the handler runs on the interrupted stack: one atomic flag only
		volatile std::sig_atomic_t g_sigwinch_flag = 0;
		struct sigaction g_saved_sigwinch = { };
		bool g_sigwinch_installed = false;

		extern "C" auto handle_sigwinch( int ) -> void {
			g_sigwinch_flag = 1;
		}

		// tv_sec is time_t, tv_usec suseconds_t: long on Linux, int on Darwin
		auto wait_for_input( const std::uint64_t remaining_ms ) -> int {
			auto read_set = fd_set{ };
			FD_ZERO( &read_set );
			FD_SET( STDIN_FILENO, &read_set );

			auto timeout = timeval{ };
			timeout.tv_sec = static_cast< decltype( timeout.tv_sec ) >( remaining_ms / 1000 );
			timeout.tv_usec = static_cast< decltype( timeout.tv_usec ) >(
				( remaining_ms % 1000 ) * 1000 );

			return ::select( STDIN_FILENO + 1, &read_set, nullptr, nullptr, &timeout );
		}

	}

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

		// SA_RESTART; select is exempt, its EINTR is handled below
		struct sigaction action{ };
		action.sa_handler = handle_sigwinch;
		action.sa_flags = SA_RESTART;
		sigemptyset( &action.sa_mask );

		if ( ::sigaction( SIGWINCH, &action, &g_saved_sigwinch ) != 0 ) {
			return std::unexpected( fail( errc::io, "sigaction failed" ) );
		}

		g_sigwinch_installed = true;

		const auto has_tty = true;
		session.caps_ = probe_capabilities( tty_environment( "COLORTERM" ),
			tty_environment( "TERM" ), tty_environment( "NO_COLOR" ) == "1", has_tty,
			tty_environment( "MCODE_AMBIGUOUS_WIDTH" ) );

		// Wrapped here and unwrapped in `restore`, the same place the mouse
		// mode is handled, so the pair cannot drift apart.
		if ( session.caps_.bracketed_paste ) {
			session.write( BRACKETED_PASTE_ENABLE );
		}

		return session;
	}

	tty_session::tty_session( tty_session&& other ) noexcept
		: caps_( other.caps_ ), raw_active_( other.raw_active_ ),
		attached_( other.attached_ ), mouse_reporting_( other.mouse_reporting_ ),
		resize_pending_( other.resize_pending_ ), saved_( other.saved_ ),
		saved_termios_( other.saved_termios_ ) {
		other.attached_ = false;
		other.saved_ = false;
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
			saved_ = other.saved_;
			saved_termios_ = other.saved_termios_;

			other.attached_ = false;
			other.saved_ = false;
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
		resize_pending_ = false;

		if ( mouse_reporting_ ) {
			mouse_reporting_ = false;
			write( MOUSE_DISABLE_SEQUENCE );
		}

		// Paired with the enable in `create`, on every path that saved the
		// console: the shell that runs next must not inherit a terminal that
		// keeps wrapping its pastes in markers.
		if ( caps_.bracketed_paste ) {
			write( BRACKETED_PASTE_DISABLE );
		}

		if ( g_sigwinch_installed ) {
			g_sigwinch_installed = false;
			::sigaction( SIGWINCH, &g_saved_sigwinch, nullptr );
		}

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
		auto state = decode_state{ };
		auto events = std::vector< key_event >{ };
		auto got_enter = false;
		auto got_eof = false;

		// One deadline for the whole line: re-arming per byte meant a slow
		// typist could never time out.
		const auto deadline = monotonic_ms( ) + wait_ms;

		while ( !got_enter && !got_eof ) {
			const auto now = monotonic_ms( );

			if ( now >= deadline ) {
				return std::nullopt;
			}

			const auto ready = wait_for_input( deadline - now );

			if ( ready <= 0 ) {
				return std::nullopt;
			}

			auto bytes = std::array< char, 256 >{ };
			const auto count = ::read( STDIN_FILENO, bytes.data( ), bytes.size( ) );

			// vmin=0/vtime=1: a zero-length read is the timer expiring, not end of input
			if ( count == 0 ) {
				continue;
			}

			if ( count < 0 ) {
				got_eof = true;

				break;
			}

			// The same decoder the prompt uses, so a bracketed paste is
			// content here too and its markers never land in the answer.
			decode_key_bytes( std::string_view{ bytes.data( ),
				static_cast< std::size_t >( count ) }, state, events, false );

			for ( const auto& event : events ) {
				if ( event.type == key_event::kind::interrupt
					|| event.type == key_event::kind::exit ) {
					// Ctrl-C abandons the read. Returning the partial line would
					// hand the caller an empty answer, which a prompt that treats
					// empty as "skip" would act on -- so an interrupt must be
					// distinguishable from an empty line, and `nullopt` is that.
					write( "\r\n" );

					return std::nullopt;
				}

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

	auto tty_session::set_mouse_reporting( const bool enabled ) -> void {
		if ( mouse_reporting_ == enabled ) {
			return;
		}

		mouse_reporting_ = enabled;

		if ( attached_ ) {
			write( enabled ? MOUSE_ENABLE_SEQUENCE : MOUSE_DISABLE_SEQUENCE );
		}
	}

	auto tty_session::poll_resize( const std::uint32_t wait_ms ) -> bool {
		const auto deadline = monotonic_ms( ) + wait_ms;

		while ( true ) {
			if ( resize_pending_ || platform::terminal_size_changed( ) ) {
				resize_pending_ = false;

				return true;
			}

			if ( !attached_ || monotonic_ms( ) >= deadline ) {
				return false;
			}

			auto slice = timeval{ 0,
				static_cast< decltype( timeval::tv_usec ) >( RESIZE_POLL_SLICE_MS * 1000 ) };

			::select( 0, nullptr, nullptr, nullptr, &slice );

			if ( g_sigwinch_flag != 0 ) {
				g_sigwinch_flag = 0;
				resize_pending_ = true;
			}
		}
	}

	auto tty_session::read_key( const std::uint32_t wait_ms ) -> key_event {
		auto event = key_event{ };

		if ( !attached_ ) {
			event.type = key_event::kind::exit;

			return event;
		}

		const auto deadline = monotonic_ms( ) + wait_ms;

		while ( true ) {
			if ( !pending_.empty( ) ) {
				auto next = pending_.front( );
				pending_.erase( pending_.begin( ) );

				return next;
			}

			const auto now = monotonic_ms( );
			const auto remaining = now >= deadline ? std::uint64_t{ 0 } : deadline - now;

			const auto ready = wait_for_input( remaining );

			// select is never restarted, so a resize interrupts the wait.
			if ( g_sigwinch_flag != 0 ) {
				g_sigwinch_flag = 0;
				resize_pending_ = true;
			}

			if ( ready > 0 ) {
				auto bytes = std::array< char, 256 >{ };
				const auto count = ::read( STDIN_FILENO, bytes.data( ), bytes.size( ) );

				// vmin=0/vtime=1: a zero-length read is the timer, not end of input
				if ( count == 0 ) {
					continue;
				}

				if ( count < 0 ) {
					event.type = key_event::kind::exit;

					return event;
				}

				decode_key_bytes( std::string_view{ bytes.data( ),
					static_cast< std::size_t >( count ) }, decode_, pending_,
					mouse_reporting_ );

				continue;
			}

			if ( ready < 0 && errno != EINTR ) {
				event.type = key_event::kind::exit;

				return event;
			}

			// The wait elapsed, or a signal cut it short. Idle is not exit:
			// the caller polls, and a decoded key still wins.
			if ( !pending_.empty( ) ) {
				continue;
			}

			if ( monotonic_ms( ) >= deadline ) {
				// A marker prefix that never completed is ordinary input, so a
				// lone Escape is still the escape key. The bytes go back
				// through the keystroke decoder, which holds an incomplete
				// escape sequence in its own carry.
				if ( decode_.paste.holding( ) ) {
					decode_plain_key_bytes( decode_.paste.flush( ), decode_, pending_,
						mouse_reporting_ );

					if ( !pending_.empty( ) ) {
						continue;
					}
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
		}
	}
}

#endif
