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

	struct capabilities {
		enum class color_depth : std::uint8_t { none, ansi16, ansi256, truecolor };

		color_depth depth = color_depth::ansi16;
		bool synchronized_output = false;
		bool kitty_keyboard = false;
		bool bracketed_paste = false;
		bool is_tty = false;

		// A mismatch with the frame builder desynchronises the grid. Exactly 1 or 2.
		std::size_t ambiguous_width = 1;
	};

	// Only "2" selects two columns; anything else, including unset, is the default.
	[[nodiscard]] auto probe_capabilities( std::string_view colorterm,
		std::string_view term, bool no_color, bool has_tty,
		std::string_view ambiguous_width_env = { } ) -> capabilities;

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
			timeout,

			escape,

			page_up,
			page_down,
			ctrl_r,
			mouse_scroll_up,
			mouse_scroll_down,

			paste,
		};

		kind type = kind::character;
		std::string text;
	};

	// DECSET 2004: the markers a terminal wraps a paste in, and their enable pair.
	inline constexpr std::string_view BRACKETED_PASTE_START = "\x1b[200~";
	inline constexpr std::string_view BRACKETED_PASTE_END = "\x1b[201~";
	inline constexpr std::string_view BRACKETED_PASTE_ENABLE = "\x1b[?2004h";
	inline constexpr std::string_view BRACKETED_PASTE_DISABLE = "\x1b[?2004l";

	class paste_decoder {
	public:
		enum class outcome : std::uint8_t {
			plain,
			incomplete,
			started,
			finished,
		};

		[[nodiscard]] auto feed( std::string_view bytes ) -> outcome;

		[[nodiscard]] auto text( ) const noexcept -> std::string_view { return text_; }

		[[nodiscard]] auto consumed( ) const noexcept -> std::size_t { return consumed_; }

		[[nodiscard]] auto active( ) const noexcept -> bool { return active_; }

		[[nodiscard]] auto holding( ) const noexcept -> bool { return !held_.empty( ); }

		[[nodiscard]] auto flush( ) -> std::string_view;

	private:
		[[nodiscard]] auto release_held( ) -> outcome;

		bool active_ = false;
		std::string held_;
		std::string paste_;
		std::string text_;
		std::size_t consumed_ = 0;
	};

	struct decode_state {
		std::string carry;
		paste_decoder paste;
	};

	auto decode_key_bytes( std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, bool mouse_reporting ) -> void;

	auto decode_plain_key_bytes( std::string_view bytes, decode_state& state,
		std::vector< key_event >& out, bool mouse_reporting ) -> void;

	[[nodiscard]] auto monotonic_ms( ) -> std::uint64_t;

	[[nodiscard]] auto tty_environment( const char* name ) -> std::string_view;

	// Restores the console on every exit path, including a throw.
	class tty_session {
	public:
		[[nodiscard]] static auto create( ) -> result< tty_session >;

		tty_session( tty_session&& other ) noexcept;
		auto operator=( tty_session&& other ) noexcept -> tty_session&;

		tty_session( const tty_session& ) = delete;
		auto operator=( const tty_session& ) -> tty_session& = delete;

		~tty_session( );

		auto write( std::string_view bytes ) -> void;

		[[nodiscard]] auto read_line( std::uint32_t wait_ms )
			-> std::optional< std::string >;

		[[nodiscard]] auto read_key( std::uint32_t wait_ms ) -> key_event;

		[[nodiscard]] auto size( ) const -> std::pair< int, int >;

		[[nodiscard]] auto resized( ) const -> bool;

		[[nodiscard]] auto poll_resize( std::uint32_t wait_ms ) -> bool;

		auto set_mouse_reporting( bool enabled ) -> void;

		[[nodiscard]] auto caps( ) const noexcept -> const capabilities& { return caps_; }

	private:
		tty_session( ) = default;

		auto restore( ) noexcept -> void;

		capabilities caps_;
		bool attached_ = false;
		bool mouse_reporting_ = false;

		bool resize_pending_ = false;

		std::vector< key_event > pending_;

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
