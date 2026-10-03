
#if !defined( _WIN32 )

#include "mcode/tui/tty.hxx"

#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "mcode/platform/seams.hxx"

namespace mcode::tui {

	namespace {

		// Raw mode disables echo, so erasing a character is the session's job:
		// backspace, blank over the cell, backspace again.
		inline constexpr std::string_view ERASE_SEQUENCE = "\b \b";

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

		const auto has_tty = true;
		session.caps_ = probe_capabilities( tty_environment( "COLORTERM" ),
			tty_environment( "TERM" ), tty_environment( "NO_COLOR" ) == "1", has_tty );

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

		// One deadline for the whole line: re-arming per byte meant a slow
		// typist could never time out.
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

		// `consumed == 0` means `text` is a prefix of a longer sequence, so the
		// caller keeps it and reads on.
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

			// Still a prefix: wait for the rest.
			for ( const auto& candidate : MAPPINGS ) {
				if ( candidate.sequence.starts_with( text ) ) {
					return result;
				}
			}

			if ( text.size( ) == 1 ) {
				result.event.type = key_event::kind::escape;
				result.consumed = 1;

				return result;
			}

			// Consume an unrecognised sequence rather than emitting its bytes.
			result.event.type = key_event::kind::timeout;
			result.consumed = text.size( );

			return result;
		}

		// Splits a raw read into whole keys. A read can deliver several keys
		// and can split one.
		auto decode_posix_bytes( const std::string_view text,
			std::string& carry, std::vector< key_event >& out ) -> void {
			carry.append( text );

			auto cursor = std::size_t{ 0 };

			while ( cursor < carry.size( ) ) {
				const auto first = static_cast< unsigned char >( carry[ cursor ] );

				if ( first == 0x1B ) {
					const auto parsed = decode_escape( std::string_view{ carry }.substr( cursor ) );

					if ( parsed.consumed == 0 ) {
						break;
					}

					cursor += parsed.consumed;

					// Swallowed rather than
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

				if ( first == 0x09 ) {
					auto event = key_event{ };
					event.type = key_event::kind::tab;

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
}

#endif
