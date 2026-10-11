#include "mcode/tui/tty.hxx"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	namespace {

		inline constexpr std::size_t PASTE_MARKER_BYTES = BRACKETED_PASTE_START.size( );

		[[nodiscard]] auto marker_progress( const std::string_view bytes )
			-> std::size_t {
			auto matched = std::size_t{ 0 };

			while ( matched < bytes.size( ) && matched < PASTE_MARKER_BYTES &&
				bytes[ matched ] == BRACKETED_PASTE_START[ matched ] ) {
				++matched;
			}

			return matched;
		}

		// Never the whole marker: a complete one would have been found.
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

		// Longer than this is not an escape sequence: the carry is dropped, not grown.
		inline constexpr std::size_t MAX_ESCAPE_BYTES = 32;

		using key_kind = key_event::kind;

		struct mapping {
			std::string_view sequence;
			key_kind kind;
			bool mouse = false;
		};

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

				const auto terminator = text.find_first_of( "Mm", candidate.sequence.size( ) );

				if ( terminator == std::string_view::npos ) {
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

			if ( text.size( ) >= 2 && text[ 1 ] != '[' && text[ 1 ] != 'O' ) {
				result.event.type = key_event::kind::escape;
				result.consumed = 1;

				return result;
			}

			const auto final = text.find_first_not_of( CSI_PARAMETERS, 2 );

			if ( final == std::string_view::npos && text.size( ) <= MAX_ESCAPE_BYTES ) {
				return result;
			}

			result.event.type = key_event::kind::timeout;
			result.consumed = final == std::string_view::npos ? text.size( ) : final + 1;

			return result;
		}

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
			const auto take = std::min( PASTE_MARKER_BYTES - held_.size( ), bytes.size( ) );

			held_.append( bytes.substr( 0, take ) );
			consumed_ = take;

			// With a paste open this completes its end marker, otherwise a start
			// marker: testing the wrong one would release a split end marker into
			// the paste.
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
			const auto found = bytes.find( BRACKETED_PASTE_END );

			if ( found == std::string_view::npos ) {
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

		// A complete start marker comes first: it is also a prefix of itself.
		if ( rest.starts_with( BRACKETED_PASTE_START ) ) {
			text_ = bytes.substr( 0, found );
			consumed_ = found + BRACKETED_PASTE_START.size( );
			paste_.clear( );
			active_ = true;

			return outcome::started;
		}

		if ( progress == rest.size( ) ) {
			text_ = bytes.substr( 0, found );
			consumed_ = bytes.size( );
			held_.assign( rest );

			return outcome::incomplete;
		}

		if ( progress < PASTE_MARKER_BYTES ) {
			text_ = bytes.substr( 0, found + 1 );
			consumed_ = found + 1;

			return outcome::plain;
		}

		text_ = bytes.substr( 0, found );
		consumed_ = found + BRACKETED_PASTE_END.size( );

		return outcome::plain;
	}

	auto paste_decoder::release_held( ) -> outcome {
		if ( active_ ) {
			paste_.append( held_ );
			held_.clear( );

			return outcome::plain;
		}

		text_ = held_;
		held_.clear( );

		return outcome::plain;
	}

	auto paste_decoder::flush( ) -> std::string_view {
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
				if ( !state.paste.text( ).empty( ) ) {
					decode_plain_key_bytes( state.paste.text( ), state, out, mouse_reporting );
				}

				return;
			}

			if ( outcome == paste_decoder::outcome::finished ) {
				// A paste is content, not keystrokes: one event, newlines and all.
				if ( !state.paste.text( ).empty( ) ) {
					auto event = key_event{ };
					event.type = key_event::kind::paste;
					event.text = normalized_paste( state.paste.text( ) );

					out.push_back( std::move( event ) );
				}
			} else if ( !state.paste.text( ).empty( ) ) {
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

				if ( parsed.event.type != key_event::kind::timeout ) {
					out.push_back( parsed.event );
				}

				continue;
			}

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

			const auto length = utf8_lead_length( carry[ cursor ] );

			if ( length == 0 ) {
				++cursor;

				continue;
			}

			if ( cursor + length > carry.size( ) ) {
				break;
			}

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
