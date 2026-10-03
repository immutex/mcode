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

	// The protocol flags are assumed for a real tty, not probed: a DECRQM
	// round-trip is out of scope, and a terminal that does not know a sequence
	// ignores it.
	[[nodiscard]] auto probe_capabilities( std::string_view colorterm,
		std::string_view term, bool no_color, bool has_tty ) -> capabilities;

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
		};

		kind type = kind::character;
		std::string text;
	};

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

		// One console read yields several records and the caller asks for one
		// key, so the surplus waits here rather than being dropped.
		std::vector< key_event > pending_;

		// A read can split an escape sequence: `\x1b[A` may arrive as `\x1b`
		// then `[A`.
		std::string carry_;

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
