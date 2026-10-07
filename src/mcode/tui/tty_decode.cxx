// Key decoding, shared by both consoles. What arrives is bytes either way --
// a POSIX terminal writes them and Windows reports them as key records -- so
// the escape table, the paste machine and the plain decoder live here once
// rather than once per platform.
#include "mcode/tui/tty.hxx"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	namespace {

		// Both markers are the same length, so one constant covers both.
		inline constexpr std::size_t PASTE_MARKER_BYTES = BRACKETED_PASTE_START.size( );

		// How far `bytes` gets into a marker before it diverges.
		[[nodiscard]] auto marker_progress( const std::string_view bytes )
			-> std::size_t {
			auto matched = std::size_t{ 0 };

			while ( matched < bytes.size( ) && matched < PASTE_MARKER_BYTES &&
				bytes[ matched ] == BRACKETED_PASTE_START[ matched ] ) {
				++matched;
			}

			return matched;
		}

		// The longest tail of `bytes` that could be the front of an end marker
		// the next read completes. Never the whole marker: a complete one would
		// have been found.
		[[nodiscard]] auto marker_suffix_length( const std::string_view bytes )
			-> std::size_t {
			const auto limit = std::min( bytes.size( ), PASTE_MARKER_BYTES - 1 );

			for ( auto length = limit; length > 0; --length ) {
				if ( bytes.substr( bytes.size( ) - length ) ==
					BRACKETED_PASTE_END.substr( 0, length ) ) {
					return length;
				}
			}

			return 0;
		}

		// Pasted text carries the terminal's own line endings, and a paste is
		// inserted as content: one newline per line, never a carriage return
		// that would ride into the input buffer.
		[[nodiscard]] auto normalized_paste( const std::string_view raw ) -> std::string {
			auto out = std::string{ };
			out.reserve( raw.size( ) );

			for ( auto index = std::size_t{ 0 }; index < raw.size( ); ++index ) {
				if ( raw[ index ] == '\r' ) {
					out.push_back( '\n' );

					// A CRLF pair is one line break, not two.
					if ( index + 1 < raw.size( ) && raw[ index + 1 ] == '\n' ) {
						++index;
					}

					continue;
				}

				out.push_back( raw[ index ] );
			}

			return out;
		}

		// before a CSI's final byte: digits, `;`, SGR `<`, private-mode `?`
		inline constexpr std::string_view CSI_PARAMETERS = "0123456789;<?";

		// An escape sequence longer than this is not one: the carry is dropped
		// rather than grown without bound.
		inline constexpr std::size_t MAX_ESCAPE_BYTES = 32;

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

	}

	auto paste_decoder::feed( const std::string_view bytes ) -> outcome {
		text_.clear( );
		consumed_ = 0;

		if ( !held_.empty( ) ) {
			// Only as much of `bytes` as the pending marker still needs, so
			// the rest stays the caller's to feed again.
			const auto take = std::min( PASTE_MARKER_BYTES - held_.size( ), bytes.size( ) );

			held_.append( bytes.substr( 0, take ) );
			consumed_ = take;

			// Whichever marker is expected: with a paste open this completes
			// its end marker, otherwise it completes a start marker. Testing
			// the wrong one would release a split end marker into the paste,
			// or let a start marker pasted as content wipe the paste it is in.
			const auto expected = active_ ? BRACKETED_PASTE_END : BRACKETED_PASTE_START;

			if ( held_.size( ) < PASTE_MARKER_BYTES ) {
				if ( expected.starts_with( held_ ) ) {
					return outcome::incomplete;
				}

				return release_held( );
			}

			if ( std::string_view{ held_ } != expected ) {
				return release_held( );
			}

			held_.clear( );

			if ( active_ ) {
				active_ = false;
				text_ = paste_;
				paste_.clear( );

				return outcome::finished;
			}

			paste_.clear( );
			active_ = true;

			return outcome::started;
		}

		if ( active_ ) {
			// While a paste is open the first end marker found closes it, so a
			// marker the user pasted survives as ordinary text and the paste
			// still ends where the terminal said it did.
			const auto found = bytes.find( BRACKETED_PASTE_END );

			if ( found == std::string_view::npos ) {
				// A suffix that could be the front of an end marker is held
				// rather than folded into the paste, because the next read may
				// complete it. Anything else is content.
				const auto hold = marker_suffix_length( bytes );

				paste_.append( bytes.substr( 0, bytes.size( ) - hold ) );

				if ( hold > 0 ) {
					held_.assign( bytes.substr( bytes.size( ) - hold ) );
				}

				consumed_ = bytes.size( );

				return outcome::plain;
			}

			paste_.append( bytes.substr( 0, found ) );
			active_ = false;
			consumed_ = found + BRACKETED_PASTE_END.size( );
			text_ = paste_;
			paste_.clear( );

			return outcome::finished;
		}

		const auto found = bytes.find( '\x1b' );

		if ( found == std::string_view::npos ) {
			text_ = bytes;
			consumed_ = bytes.size( );

			return outcome::plain;
		}

		const auto rest = bytes.substr( found );
		const auto progress = marker_progress( rest );

		// A complete start marker comes first: it is also a prefix of itself,
		// and testing the prefix case before it would hold the marker forever.
		if ( rest.starts_with( BRACKETED_PASTE_START ) ) {
			text_ = bytes.substr( 0, found );
			consumed_ = found + BRACKETED_PASTE_START.size( );
			paste_.clear( );
			active_ = true;

			return outcome::started;
		}

		if ( progress == rest.size( ) ) {
			// A marker prefix and nothing more: the next read may finish it,
			// so all of it is held and there is nothing left to feed.
			text_ = bytes.substr( 0, found );
			consumed_ = bytes.size( );
			held_.assign( rest );

			return outcome::incomplete;
		}

		if ( progress < PASTE_MARKER_BYTES ) {
			// Ordinary input up to and including the escape; the rest of
			// `bytes` is fed again by the caller.
			text_ = bytes.substr( 0, found + 1 );
			consumed_ = found + 1;

			return outcome::plain;
		}

		// An end marker with no paste open is stray input: dropped, because a
		// terminal that never saw the start cannot have pasted anything.
		text_ = bytes.substr( 0, found );
		consumed_ = found + BRACKETED_PASTE_END.size( );

		return outcome::plain;
	}

	auto paste_decoder::release_held( ) -> outcome {
		if ( active_ ) {
			// Bytes held for a marker that turned out not to be one are paste
			// content: decoding them as keys would submit their newlines.
			paste_.append( held_ );
			held_.clear( );

			return outcome::plain;
		}

		text_ = held_;
		held_.clear( );

		return outcome::plain;
	}

	auto paste_decoder::flush( ) -> std::string_view {
		// An open paste keeps its candidate: those bytes are content, and they
		// may still be the front of the end marker that closes the paste.
		if ( active_ ) {
			return { };
		}

		if ( held_.empty( ) ) {
			return { };
		}

		text_ = held_;
		held_.clear( );

		return text_;
	}

	auto decode_key_bytes( const std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, const bool mouse_reporting ) -> void {
		auto rest = bytes;

		while ( true ) {
			const auto outcome = state.paste.feed( rest );

			if ( outcome == paste_decoder::outcome::incomplete ) {
				return;
			}

			if ( outcome == paste_decoder::outcome::finished ) {
				// A paste is content, not keystrokes: one event, newlines and
				// all. Nothing to report when the terminal pasted nothing.
				if ( !state.paste.text( ).empty( ) ) {
					auto event = key_event{ };
					event.type = key_event::kind::paste;
					event.text = normalized_paste( state.paste.text( ) );

					out.push_back( std::move( event ) );
				}
			} else if ( !state.paste.text( ).empty( ) ) {
				// Ordinary bytes, or the input that preceded a start marker.
				decode_plain_key_bytes( state.paste.text( ), state, out, mouse_reporting );
			}

			rest.remove_prefix( state.paste.consumed( ) );

			if ( rest.empty( ) ) {
				return;
			}
		}
	}

	auto decode_plain_key_bytes( const std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, const bool mouse_reporting ) -> void {
		auto& carry = state.carry;

		carry.append( bytes );

		auto cursor = std::size_t{ 0 };

		while ( cursor < carry.size( ) ) {
			const auto first = static_cast< unsigned char >( carry[ cursor ] );

			if ( first == 0x1B ) {
				const auto parsed = decode_escape( std::string_view{ carry }.substr( cursor ),
					mouse_reporting );

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
			const auto length = utf8_lead_length( carry[ cursor ] );

			if ( length == 0 ) {
				// a byte that cannot start a sequence: drop it rather than insert half a glyph
				++cursor;

				continue;
			}

			if ( cursor + length > carry.size( ) ) {
				break;
			}

			// the strict decoder rejects a bad continuation, an overlong form and a surrogate
			const auto decoded = decode_utf8( std::string_view{ carry }.substr( cursor ) );

			if ( decoded.length != length ) {
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
