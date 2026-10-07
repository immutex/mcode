#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <utility>
#include <vector>

#include "mcode/core/error.hxx"

#if !defined( _WIN32 )
#include <termios.h>
#endif

namespace mcode::tui {

	// What the terminal can do, probed once at startup and cached. The
	// renderer reads this, never the environment again.
	struct capabilities {
		enum class color_depth : std::uint8_t { none, ansi16, ansi256, truecolor };

		color_depth depth = color_depth::ansi16;
		bool synchronized_output = false;
		bool kitty_keyboard = false;
		bool bracketed_paste = false;
		bool is_tty = false;

		// Display columns an East Asian *ambiguous* glyph occupies here. A
		// terminal in an East Asian width mode renders `│`, `─`, `•` and `…`
		// two columns wide; a mismatch with the frame builder desynchronises
		// the grid. Exactly 1 or 2.
		std::size_t ambiguous_width = 1;
	};

	// The protocol flags are assumed for a real tty, not probed: a DECRQM
	// round-trip is out of scope, and a terminal that does not know a sequence
	// ignores it. `ambiguous_width_env` is the raw `MCODE_AMBIGUOUS_WIDTH`
	// value: only "2" selects two columns, and anything else, including unset,
	// is the one-column default. It is a parameter, not a `std::getenv` call,
	// so the probe stays pure and testable.
	[[nodiscard]] auto probe_capabilities( std::string_view colorterm,
		std::string_view term, bool no_color, bool has_tty,
		std::string_view ambiguous_width_env = { } ) -> capabilities;

	// One decoded key from the raw input stream.
	struct key_event {
		enum class kind : std::uint8_t {
			character,
			enter,
			tab,
			backspace,
			delete_key,
			left,
			right,
			up,
			down,
			home,
			end,
			interrupt,
			exit,

			// The wait elapsed with no key. Idle is not exit.
			timeout,

			// A bare Escape: abandons the input. Ctrl+D is `exit`.
			escape,

			// The view moved a screenful. The live region scrolls itself;
			// these never reach the terminal's own scrollback.
			page_up,
			page_down,

			// Ctrl+R: reverse search over the session history.
			ctrl_r,

			// Wheel notches, reported only while mouse reporting is on.
			mouse_scroll_up,
			mouse_scroll_down,

			// One bracketed paste with its markers stripped. The text is
			// content, not keystrokes: the caller inserts it without
			// submitting, so the newlines inside it survive.
			paste,
		};

		kind type = kind::character;
		std::string text;
	};

	// The sequences a terminal wraps a paste in while DECSET 2004 is on, and
	// the pair that turns that wrapping on and off.
	inline constexpr std::string_view BRACKETED_PASTE_START = "\x1b[200~";
	inline constexpr std::string_view BRACKETED_PASTE_END = "\x1b[201~";
	inline constexpr std::string_view BRACKETED_PASTE_ENABLE = "\x1b[?2004h";
	inline constexpr std::string_view BRACKETED_PASTE_DISABLE = "\x1b[?2004l";

	// Splits a terminal's byte stream into ordinary input and bracketed
	// pastes. A marker can be split across two reads, and a marker the user
	// pasted must not be mistaken for a real one, so this runs ahead of the
	// keystroke decoder and holds whatever is still undecided. One
	// implementation, so no platform can disagree about what a marker is.
	class paste_decoder {
	public:
		enum class outcome : std::uint8_t {
			// ordinary input: decode `text` normally
			plain,

			// a marker prefix: everything fed so far is held
			incomplete,

			// the start marker: a paste is open
			started,

			// the end marker: `text` is the paste
			finished,
		};

		// Consumes the head of `bytes`. `consumed` is how much of `bytes` the
		// outcome accounts for, so the caller drops exactly that much and
		// feeds the rest again. `text` is valid until the next call.
		[[nodiscard]] auto feed( std::string_view bytes ) -> outcome;

		// What the last `feed` reported: ordinary bytes, or the paste.
		[[nodiscard]] auto text( ) const noexcept -> std::string_view { return text_; }

		// How many bytes of the last `feed` argument it accounts for.
		[[nodiscard]] auto consumed( ) const noexcept -> std::size_t { return consumed_; }

		// True while a paste is open.
		[[nodiscard]] auto active( ) const noexcept -> bool { return active_; }

		// True while a marker prefix is held: the next byte may continue it.
		[[nodiscard]] auto holding( ) const noexcept -> bool { return !held_.empty( ); }

		// Releases a held marker prefix when a wait expires, so a lone Escape
		// is not held forever. An open paste keeps its candidate: those bytes
		// are content, and they may still be the front of the end marker.
		[[nodiscard]] auto flush( ) -> std::string_view;

	private:
		// A held prefix that diverged from the marker being looked for is
		// ordinary input -- or paste content, when a paste is open.
		[[nodiscard]] auto release_held( ) -> outcome;

		bool active_ = false;
		std::string held_;
		std::string paste_;
		std::string text_;
		std::size_t consumed_ = 0;
	};

	// The half-decoded tail of a read. A read can split an escape sequence,
	// and a bracketed paste's end marker can land in a later read, so the
	// decoders keep their state here instead of in local variables.
	struct decode_state {
		std::string carry;
		paste_decoder paste;
	};

	// Decodes a terminal's byte stream into keys, appending to `out`. The
	// paste machine runs first, so a paste arrives as one event and its
	// markers never reach the keystroke decoder. One implementation, so the
	// two platforms cannot disagree about what a marker is.
	auto decode_key_bytes( std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, bool mouse_reporting ) -> void;

	// One keystroke decoder for both platforms: the bytes are the same bytes
	// whichever console produced them, and a second implementation would be a
	// second thing to keep in step. Used by `decode_key_bytes` on input that
	// is not part of a paste, and directly by a wait that releases a partial
	// marker.
	auto decode_plain_key_bytes( std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, bool mouse_reporting ) -> void;

	// Milliseconds from a monotonic clock. One definition, so the timeout
	// arithmetic is identical on every platform.
	[[nodiscard]] auto monotonic_ms( ) -> std::uint64_t;

	// An environment variable's value, or empty when unset.
	[[nodiscard]] auto tty_environment( const char* name ) -> std::string_view;

	// Owns raw mode and restores the console on every exit path, including a
	// throw. Writes are byte strings; there is no formatting layer.
	class tty_session {
	public:
		// Enables raw mode and VT processing. Fails closed.
		[[nodiscard]] static auto create( ) -> result< tty_session >;

		tty_session( tty_session&& other ) noexcept;
		auto operator=( tty_session&& other ) noexcept -> tty_session&;

		tty_session( const tty_session& ) = delete;
		auto operator=( const tty_session& ) -> tty_session& = delete;

		// Restores the console state. Never throws.
		~tty_session( );

		// Raw bytes out. Flushes.
		auto write( std::string_view bytes ) -> void;

		// One decoded line, or nothing within the wait.
		[[nodiscard]] auto read_line( std::uint32_t wait_ms )
			-> std::optional< std::string >;

		// One key event, or nothing within the wait.
		[[nodiscard]] auto read_key( std::uint32_t wait_ms ) -> key_event;

		// Terminal size in cells, through the platform seam.
		[[nodiscard]] auto size( ) const -> std::pair< int, int >;

		// True once since the last call when the terminal was resized.
		[[nodiscard]] auto resized( ) const -> bool;

		// Blocks briefly; true when the terminal size changed since the last
		// call. Event-driven where the platform reports it, with the
		// size-diff check as a fallback.
		[[nodiscard]] auto poll_resize( std::uint32_t wait_ms ) -> bool;

		// SGR mouse reporting, wheel only. Off by default: a terminal that
		// reports the wheel stops scrolling its own scrollback.
		auto set_mouse_reporting( bool enabled ) -> void;

		[[nodiscard]] auto mouse_reporting( ) const noexcept -> bool {
			return mouse_reporting_;
		}

		// The probed capabilities for this session.
		[[nodiscard]] auto caps( ) const noexcept -> const capabilities& { return caps_; }

		// Raw mode is active. A query, not a guarantee of display state.
		[[nodiscard]] auto raw_active( ) const noexcept -> bool { return raw_active_; }

	private:
		tty_session( ) = default;

		auto restore( ) noexcept -> void;

		capabilities caps_;
		bool raw_active_ = false;
		bool attached_ = false;
		bool mouse_reporting_ = false;

		// Set by a platform resize event and consumed by `poll_resize`.
		bool resize_pending_ = false;

		// One console read yields several records and the caller asks for one
		// key, so the surplus waits here rather than being dropped.
		std::vector< key_event > pending_;

		// A read can split an escape sequence, and a bracketed paste can
		// straddle two reads.
		decode_state decode_;

#if defined( _WIN32 )
		void* input_handle_ = nullptr;
		void* output_handle_ = nullptr;
		unsigned long in_mode_ = 0;
		unsigned long out_mode_ = 0;
		unsigned int saved_output_cp_ = 0;
		unsigned int saved_input_cp_ = 0;
		bool saved_ = false;
#else
		bool saved_ = false;
		termios saved_termios_{ };
#endif
	};

}
