#pragma once

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
	};

	// Environment-derived capability detection. The probe never writes to
	// the terminal: COLORTERM/TERM/NO_COLOR answer colour. The protocol
	// features (synchronized output, bracketed paste, Kitty keyboard) are
	// ASSUMED for a real tty, not probed -- a DECRQM round-trip is out of
	// scope for this slice, and the flags are only claims the renderer uses
	// to choose escape sequences a modern terminal ignores gracefully.
	[[nodiscard]] auto probe_capabilities( std::string_view colorterm,
		std::string_view term, bool no_color, bool has_tty ) -> capabilities;

	// One decoded key from the raw input stream.
	struct key_event {
		enum class kind : std::uint8_t {
			character,
			enter,
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
			paste,

			// The wait elapsed with no key. Distinct from `exit`: an idle
			// prompt polls, it does not end the session.
			timeout,
		};

		kind type = kind::character;
		std::string text;
	};

	// The tty session. Owns raw mode and the VT state transitions; the guard
	// restores the console on every exit path, including a throw.
	//
	// Every write is a byte string; there is no formatting layer here. The
	// platform branch lives entirely inside this class.
	class tty_session {
	public:
		// Configures raw mode and enables VT processing. Fails closed: a
		// session that cannot attach reports the error and writes nothing.
		[[nodiscard]] static auto create( ) -> result< tty_session >;

		tty_session( tty_session&& other ) noexcept;
		auto operator=( tty_session&& other ) noexcept -> tty_session&;

		tty_session( const tty_session& ) = delete;
		auto operator=( const tty_session& ) -> tty_session& = delete;

		// Restores the console state. Never throws.
		~tty_session( );

		// Raw bytes out. Flushes.
		auto write( std::string_view bytes ) -> void;

		// One line of decoded input, or nothing within the wait. A closed
		// stdin reports the closed flag; the caller decides what that means.
		[[nodiscard]] auto read_line( std::uint32_t wait_ms )
			-> std::optional< std::string >;

		// One key event, or nothing within the wait. The editor's lowest
		// layer: printable characters arrive as text, control keys as their
		// named kind. End-of-input is `exit` on an empty buffer's caller.
		[[nodiscard]] auto read_key( std::uint32_t wait_ms ) -> key_event;

		// Terminal size in cells, through the platform seam.
		[[nodiscard]] auto size( ) const -> std::pair< int, int >;

		// True once since the last call when the terminal was resized.
		[[nodiscard]] auto resized( ) const -> bool;

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

#if defined( _WIN32 )
		void* input_handle_ = nullptr;
		void* output_handle_ = nullptr;
		unsigned long in_mode_ = 0;
		unsigned long out_mode_ = 0;
		bool saved_ = false;
#else
		bool saved_ = false;
		termios saved_termios_{ };
#endif
	};

	// Turns raw input bytes into plain text lines. The incremental decoder
	// buffers partial UTF-8 and VT sequences across read boundaries; a
	// complete line is delivered when Enter arrives. Kept pure so tests can
	// feed byte chunks without a terminal.
	class input_decoder {
	public:
		// Feeds raw bytes. Returns the completed lines, in order.
		auto feed( std::string_view bytes ) -> std::vector< std::string >;

		// True when the stream reported end-of-input (Ctrl+D on an empty
		// line, or stdin closed).
		[[nodiscard]] auto closed( ) const noexcept -> bool { return closed_; }

		// The pending partial line, for the editor to render.
		[[nodiscard]] auto pending( ) const noexcept -> const std::string& { return pending_; }

	private:
		std::string pending_;
		std::string partial_sequence_;
		bool closed_ = false;
	};

}
