#include "mcode/tui/notify.hxx"

#include <cstdio>

#include "mcode/tui/tty.hxx"

#if defined( _WIN32 )
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mcode::tui {

	namespace {

		// OSC 9 is the sequence; BEL both terminates it and is the fallback
		// on a terminal that has no OSC handler.
		inline constexpr std::string_view OSC9_PREFIX = "\x1b]9;";
		inline constexpr std::string_view BELL = "\x07";

		// The title is carried into the body, because a terminal titles its
		// own notifications and would otherwise swallow the message.
		inline constexpr std::string_view TITLE_SEPARATOR = ": ";

		// A C0 byte inside the payload would either end the sequence early or
		// reach the terminal as a stray control code.
		[[nodiscard]] auto printable( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			for ( const auto byte : text ) {
				const auto value = static_cast< unsigned char >( byte );

				out.push_back( value < 0x20 || value == 0x7F ? ' ' : byte );
			}

			return out;
		}

		[[nodiscard]] auto stdout_is_terminal( ) -> bool {
		#if defined( _WIN32 )
			return _isatty( _fileno( stdout ) ) != 0;
		#else
			return ::isatty( STDOUT_FILENO ) != 0;
		#endif
		}

		// OSC 9 needs a terminal that parses escapes at all. `TERM=dumb` says
		// it does not; an unset `TERM` is no claim either way, and on Windows
		// it is unset in every console.
		[[nodiscard]] auto osc9_supported( ) -> bool {
			if ( tty_environment( "TERM" ) == "dumb" ) {
				return false;
			}

		#if defined( _WIN32 )
			auto mode = static_cast< unsigned long >( 0 );

			// A console with VT processing off prints the sequence literally.
			return GetConsoleMode( GetStdHandle( STD_OUTPUT_HANDLE ), &mode ) != 0 &&
				( mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING ) != 0;
		#else
			return true;
		#endif
		}

	}

	auto notification_bytes( const std::string_view title, const std::string_view body,
		const notification_context& context ) -> std::string {
		// Suppressed entirely: not a terminal, or the user asked for no
		// escapes. Even a bell would be bytes a consumer has to skip.
		if ( !context.stdout_is_terminal || context.no_color ) {
			return { };
		}

		if ( !context.osc9_supported ) {
			return std::string{ BELL };
		}

		return std::string{ OSC9_PREFIX } + printable( title ) +
			std::string{ TITLE_SEPARATOR } + printable( body ) + std::string{ BELL };
	}

	auto notify_terminal( const std::string_view title, const std::string_view body ) -> void {
		auto context = notification_context{ };
		context.stdout_is_terminal = stdout_is_terminal( );
		context.no_color = !tty_environment( "NO_COLOR" ).empty( );
		context.osc9_supported = osc9_supported( );

		const auto bytes = notification_bytes( title, body, context );

		if ( bytes.empty( ) ) {
			return;
		}

		// stderr, never stdout: stdout belongs to the machine-readable output.
		std::fwrite( bytes.data( ), 1, bytes.size( ), stderr );
		std::fflush( stderr );
	}

}
