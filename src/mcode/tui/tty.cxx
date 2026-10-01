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

	namespace {

		// Milliseconds from a monotonic clock, for the wait deadline. Shared by
		// both branches so their timeout arithmetic cannot drift apart.
		auto monotonic_ms( ) -> std::uint64_t {
			return static_cast< std::uint64_t >(
				std::chrono::duration_cast< std::chrono::milliseconds >(
					std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( ) );
		}

	}

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
		out_mode_( other.out_mode_ ), saved_output_cp_( other.saved_output_cp_ ),
		saved_( other.saved_ ) {
		other.attached_ = false;
		other.saved_ = false;
		other.saved_output_cp_ = 0;
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
			saved_ = other.saved_;

			other.attached_ = false;
			other.saved_ = false;
			other.saved_output_cp_ = 0;
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

		if ( saved_output_cp_ != 0 ) {
			SetConsoleOutputCP( saved_output_cp_ );
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

				if ( key.uChar.AsciiChar == '\b' ) {
					// Backspace edits the line, so a typo can be corrected
					// before Enter commits it.
					if ( !line.empty( ) ) {
						line.pop_back( );
					}

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

	namespace {

		// One console record to zero or more keys.
		//
		// A record carries `wRepeatCount`, which is how Windows reports a held
		// key: ONE record standing for N presses. Ignoring it made a held key
		// fire once.
		auto decode_windows_key( const KEY_EVENT_RECORD& key ) -> std::vector< key_event > {
			auto out = std::vector< key_event >{ };
			auto event = key_event{ };

			switch ( key.wVirtualKeyCode ) {
				case VK_RETURN: event.type = key_event::kind::enter; break;
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

		// One deadline for the whole line, not one per byte: re-arming the wait
		// on every byte meant a slow typist could never time out, and a prompt
		// that had been answered still waited on the next key.
		const auto deadline = monotonic_ms( ) + wait_ms;

		while ( !got_enter && !got_eof ) {
			const auto now = monotonic_ms( );

			if ( now >= deadline ) {
				return std::nullopt;
			}

			const auto remaining = deadline - now;

			auto read_set = fd_set{ };
			FD_ZERO( &read_set );
			FD_SET( STDIN_FILENO, &read_set );

			// Cast to each member's own type: `tv_sec` is `time_t` and
			// `tv_usec` is `suseconds_t`, which is `long` on Linux and `int`
			// on Darwin, so naming either one explicitly is wrong on the other.
			auto timeout = timeval{ };
			timeout.tv_sec = static_cast< decltype( timeout.tv_sec ) >( remaining / 1000 );
			timeout.tv_usec = static_cast< decltype( timeout.tv_usec ) >(
				( remaining % 1000 ) * 1000 );

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

			// Backspace edits the line. Without this a typo in an approval
			// answer could not be corrected -- 0x7F was appended as a literal
			// byte and the answer never matched.
			if ( byte == 0x7F || byte == 0x08 ) {
				if ( !line.empty( ) ) {
					line.pop_back( );
				}

				continue;
			}

			// A control byte is not an answer; ignoring it beats storing it.
			if ( static_cast< unsigned char >( byte ) < 0x20 ) {
				continue;
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

#if !defined( _WIN32 )

		struct escape_result {
			key_event event;
			std::size_t consumed = 0;
		};

		// One escape sequence from the front of `text`.
		//
		// `consumed == 0` means the text is a prefix of a sequence that needs
		// more bytes, so the caller keeps it and reads on. Returning a bogus
		// key instead is what made a split arrow key end the session.
		auto decode_escape( const std::string_view text ) -> escape_result {
			struct mapping {
				std::string_view sequence;
				key_event::kind kind;
			};

			static constexpr mapping MAPPINGS[] = {
				{ "\x1b[A", key_event::kind::up },
				{ "\x1b[B", key_event::kind::down },
				{ "\x1b[C", key_event::kind::right },
				{ "\x1b[D", key_event::kind::left },
				{ "\x1b[H", key_event::kind::home },
				{ "\x1b[F", key_event::kind::end },
				{ "\x1b[1~", key_event::kind::home },
				{ "\x1b[4~", key_event::kind::end },
				{ "\x1b[3~", key_event::kind::delete_key },
				{ "\x1bOA", key_event::kind::up },
				{ "\x1bOB", key_event::kind::down },
				{ "\x1bOC", key_event::kind::right },
				{ "\x1bOD", key_event::kind::left },
			};

			auto result = escape_result{ };

			for ( const auto& candidate : MAPPINGS ) {
				if ( text.starts_with( candidate.sequence ) ) {
					result.event.type = candidate.kind;
					result.consumed = candidate.sequence.size( );

					return result;
				}
			}

			// Still a prefix of something longer: wait for the rest. A lone
			// ESC is only "exit" once nothing follows it.
			for ( const auto& candidate : MAPPINGS ) {
				if ( candidate.sequence.starts_with( text ) ) {
					return result;
				}
			}

			if ( text.size( ) == 1 ) {
				// A bare ESC, with nothing after it.
				result.event.type = key_event::kind::escape;
				result.consumed = 1;

				return result;
			}

			// An unrecognized sequence: consume it rather than emitting its
			// bytes as text.
			result.event.type = key_event::kind::timeout;
			result.consumed = text.size( );

			return result;
		}

		// Splits a raw read into whole keys.
		//
		// The previous version compared the ENTIRE read against exact escape
		// strings and returned at most one key, so any read that was not
		// exactly one known sequence fell through to "control byte" and quit
		// the session. Fast typing or a paste delivered several keys in one
		// read and ended the session.
		auto decode_posix_bytes( const std::string_view text,
			std::string& carry, std::vector< key_event >& out ) -> void {
			carry.append( text );

			auto cursor = std::size_t{ 0 };

			while ( cursor < carry.size( ) ) {
				const auto first = static_cast< unsigned char >( carry[ cursor ] );

				if ( first == 0x1B ) {
					// A sequence needs its final byte before it can be named.
					// Without one buffered, the next read completes it.
					const auto parsed = decode_escape( std::string_view{ carry }.substr( cursor ) );

					if ( parsed.consumed == 0 ) {
						break;
					}

					cursor += parsed.consumed;

					// An unrecognized sequence is swallowed rather than
					// delivered: `timeout` here means "nothing to report", and
					// pushing it would make the caller repaint for a key the
					// user never pressed.
					if ( parsed.event.type != key_event::kind::timeout ) {
						out.push_back( parsed.event );
					}

					continue;
				}

				if ( first == '\r' || first == '\n' ) {
					auto event = key_event{ };
					event.type = key_event::kind::enter;

					out.push_back( event );
					++cursor;

					continue;
				}

				if ( first == 0x7F || first == 0x08 ) {
					auto event = key_event{ };
					event.type = key_event::kind::backspace;

					out.push_back( event );
					++cursor;

					continue;
				}

				if ( first == 0x03 ) {
					auto event = key_event{ };
					event.type = key_event::kind::interrupt;

					out.push_back( event );
					++cursor;

					continue;
				}

				if ( first == 0x04 ) {
					auto event = key_event{ };
					event.type = key_event::kind::exit;

					out.push_back( event );
					++cursor;

					continue;
				}

				// A UTF-8 character: take the whole sequence, and wait for the
				// rest if this read split it. Emitting the raw bytes as one
				// "character" was fine for ASCII and wrong for everything else.
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

				if ( length == 1 && first < 0x20 ) {
					// An unhandled control byte is not text; ignoring it is
					// better than ending the session, which is what the old
					// fall-through did.
					++cursor;

					continue;
				}

				auto event = key_event{ };
				event.type = key_event::kind::character;
				event.text.assign( carry, cursor, length );

				out.push_back( std::move( event ) );
				cursor += length;
			}

			carry.erase( 0, cursor );
		}

#endif

	}

	auto tty_session::read_key( const std::uint32_t wait_ms ) -> key_event {
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

		const auto deadline = monotonic_ms( ) + wait_ms;

		while ( true ) {
			if ( !pending_.empty( ) ) {
				auto next = pending_.front( );
				pending_.erase( pending_.begin( ) );

				return next;
			}

			auto read_set = fd_set{ };
			FD_ZERO( &read_set );
			FD_SET( STDIN_FILENO, &read_set );

			const auto remaining = monotonic_ms( ) >= deadline
				? std::uint64_t{ 0 }
				: deadline - monotonic_ms( );

			auto timeout = timeval{ };
			timeout.tv_sec = static_cast< decltype( timeout.tv_sec ) >( remaining / 1000 );
			timeout.tv_usec = static_cast< decltype( timeout.tv_usec ) >(
				( remaining % 1000 ) * 1000 );

			const auto ready = ::select( STDIN_FILENO + 1, &read_set, nullptr, nullptr,
				&timeout );

			if ( ready == 0 ) {
				// The wait elapsed. Idle is not exit: the caller polls.
				if ( !pending_.empty( ) ) {
					continue;
				}

				event.type = key_event::kind::timeout;

				return event;
			}

			if ( ready < 0 ) {
				event.type = key_event::kind::exit;

				return event;
			}

			auto bytes = std::array< char, 256 >{ };
			const auto count = ::read( STDIN_FILENO, bytes.data( ), bytes.size( ) );

			if ( count <= 0 ) {
				event.type = key_event::kind::exit;

				return event;
			}

			decode_posix_bytes( std::string_view{ bytes.data( ),
				static_cast< std::size_t >( count ) }, carry_, pending_ );
		}
	}


#endif


}
