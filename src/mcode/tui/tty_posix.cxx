
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

		inline constexpr std::size_t MAX_ESCAPE_BYTES = 32;

		// before a CSI's final byte: digits, `;`, SGR `<`, private-mode `?`
		inline constexpr std::string_view CSI_PARAMETERS = "0123456789;<?";

		// How long `poll_resize` waits between checks of the flag.
		inline constexpr int RESIZE_POLL_SLICE_MS = 10;

		using key_kind = key_event::kind;

		struct mapping {
			std::string_view sequence;
			key_kind kind;
			bool mouse = false;
		};

		// Whole sequences, matched before any parameter is parsed.
		inline constexpr mapping MAPPINGS[] = {
			{ "\x1b[A", key_kind::up }, { "\x1b[B", key_kind::down },
			{ "\x1b[C", key_kind::right }, { "\x1b[D", key_kind::left },
			{ "\x1bOA", key_kind::up }, { "\x1bOB", key_kind::down },
			{ "\x1bOC", key_kind::right }, { "\x1bOD", key_kind::left },
			{ "\x1b[H", key_kind::home }, { "\x1b[F", key_kind::end },
			{ "\x1b[1~", key_kind::home }, { "\x1b[7~", key_kind::home },
			{ "\x1b[4~", key_kind::end }, { "\x1b[8~", key_kind::end },
			{ "\x1b[3~", key_kind::delete_key }, { "\x1b[5~", key_kind::page_up },
			{ "\x1b[6~", key_kind::page_down }, { "\x1b[1;2A", key_kind::page_up },
			{ "\x1b[1;2B", key_kind::page_down },
			// SGR wheel reports, whose coordinates follow the button
			{ "\x1b[<64;", key_kind::mouse_scroll_up, true },
			{ "\x1b[<65;", key_kind::mouse_scroll_down, true },
		};

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

			auto byte = char{ };
			const auto count = ::read( STDIN_FILENO, &byte, 1 );

			if ( count <= 0 ) {
				got_eof = true;

				break;
			}

			if ( byte == '\r' || byte == '\n' ) {
				// The caller advances its own row, so the key itself only
				// returns the cursor to column zero.
				write( "\r\n" );

				got_enter = true;

				break;
			}

			if ( byte == 0x7F || byte == 0x08 ) {
				if ( !line.empty( ) ) {
					line.pop_back( );

					write( ERASE_SEQUENCE );
				}

				continue;
			}

			if ( static_cast< unsigned char >( byte ) < 0x20 ) {
				continue;
			}

			line.push_back( byte );

			write( std::string_view{ &byte, 1 } );
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

	namespace {

		// How many bytes the UTF-8 sequence starting with `lead` occupies.
		// Zero for a continuation byte, which cannot start one.
		auto utf8_length( const unsigned char lead ) -> std::size_t {
			if ( ( lead & 0x80 ) == 0 ) {
				return 1;
			}

			if ( ( lead & 0xE0 ) == 0xC0 ) {
				return 2;
			}

			if ( ( lead & 0xF0 ) == 0xE0 ) {
				return 3;
			}

			if ( ( lead & 0xF8 ) == 0xF0 ) {
				return 4;
			}

			return 0;
		}


		struct escape_result {
			key_event event;
			std::size_t consumed = 0;
		};

		// `consumed == 0` means `text` is a prefix: the caller keeps it.
		auto decode_escape( const std::string_view text, const bool mouse_reporting )
			-> escape_result {
			auto result = escape_result{ };

			for ( const auto& candidate : MAPPINGS ) {
				// A prefix waits, so a split `ESC [ 5 ~` stays whole.
				if ( !text.starts_with( candidate.sequence ) ) {
					if ( candidate.sequence.starts_with( text ) ) {
						return result;
					}

					continue;
				}

				if ( !candidate.mouse ) {
					result.event.type = candidate.kind;
					result.consumed = candidate.sequence.size( );

					return result;
				}

				// the coordinates after the button are read and discarded
				const auto terminator = text.find_first_of( "Mm", candidate.sequence.size( ) );

				if ( terminator == std::string_view::npos ) {
					// A report that never ends must not grow the carry.
					if ( text.size( ) > MAX_ESCAPE_BYTES ) {
						result.event.type = key_event::kind::timeout;
						result.consumed = text.size( );
					}

					return result;
				}

				result.consumed = terminator + 1;
				result.event.type = mouse_reporting ? candidate.kind
					: key_event::kind::timeout;

				return result;
			}

			// an ESC before a non-sequence byte is the escape key itself
			if ( text.size( ) >= 2 && text[ 1 ] != '[' && text[ 1 ] != 'O' ) {
				result.event.type = key_event::kind::escape;
				result.consumed = 1;

				return result;
			}

			// a CSI still collecting parameters waits for its final byte
			const auto final = text.find_first_not_of( CSI_PARAMETERS, 2 );

			if ( final == std::string_view::npos && text.size( ) <= MAX_ESCAPE_BYTES ) {
				return result;
			}

			result.event.type = key_event::kind::timeout;
			result.consumed = final == std::string_view::npos ? text.size( ) : final + 1;

			return result;
		}

		// A single control byte, or nothing. `ESC` is handled before this.
		auto control_kind( const unsigned char byte ) -> std::optional< key_kind > {
			switch ( byte ) {
				case '\r': case '\n': return key_kind::enter;
				case 0x09: return key_kind::tab;
				case 0x7F: case 0x08: return key_kind::backspace;
				case 0x03: return key_kind::interrupt;
				case 0x04: return key_kind::exit;
				case 0x12: return key_kind::ctrl_r;
				default: return std::nullopt;
			}
		}

		// Splits a raw read into whole keys. A read can deliver several keys
		// and can split one.
		auto decode_posix_bytes( const std::string_view text, std::string& carry,
			std::vector< key_event >& out, const bool mouse_reporting ) -> void {
			carry.append( text );

			auto cursor = std::size_t{ 0 };

			while ( cursor < carry.size( ) ) {
				const auto first = static_cast< unsigned char >( carry[ cursor ] );

				if ( first == 0x1B ) {
					const auto parsed = decode_escape(
						std::string_view{ carry }.substr( cursor ), mouse_reporting );

					if ( parsed.consumed == 0 ) {
						break;
					}

					cursor += parsed.consumed;

					// Swallowed: `timeout` here means nothing to report.
					if ( parsed.event.type != key_event::kind::timeout ) {
						out.push_back( parsed.event );
					}

					continue;
				}

				// A control byte maps to one key, or to nothing.
				const auto control = control_kind( first );

				if ( control ) {
					auto event = key_event{ };
					event.type = *control;

					out.push_back( event );
					++cursor;

					continue;
				}

				if ( first < 0x20 ) {
					++cursor;

					continue;
				}

				// a UTF-8 character, waiting if this read split the sequence
				const auto length = utf8_length( first );

				if ( length == 0 ) {
					// A stray continuation byte: drop it rather than insert
					// half a character.
					++cursor;

					continue;
				}

				if ( cursor + length > carry.size( ) ) {
					break;
				}

				auto event = key_event{ };
				event.type = key_event::kind::character;
				event.text.assign( carry, cursor, length );

				out.push_back( std::move( event ) );
				cursor += length;
			}

			carry.erase( 0, cursor );
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

				if ( count <= 0 ) {
					event.type = key_event::kind::exit;

					return event;
				}

				decode_posix_bytes( std::string_view{ bytes.data( ),
					static_cast< std::size_t >( count ) }, carry_, pending_, mouse_reporting_ );

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
				// an ESC held back for a sequence that never arrived is the key
				if ( carry_ == "\x1b" ) {
					carry_.clear( );
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
