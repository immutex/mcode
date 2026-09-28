#include "mcode/platform/seams.hxx"

#include <cstdlib>
#include <string>
#include <utility>

#if defined( _WIN32 )
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <csignal>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace mcode::platform {

	// M0 ships interfaces and honest stubs. Where a platform cannot do the thing
	// yet, the call returns `unsupported` and `sandbox_support_level` says so --
	// a stub that silently succeeded would let a caller believe it was isolated.

	auto pty_session::supported( ) noexcept -> bool {
	#if defined( _WIN32 )
		// ConPTY is a Windows 10 1809+ API and is present in the kernel32 export
		// table on every supported version.
		return true;
	#else
		return true;
	#endif
	}

	auto pty_session::spawn( const std::filesystem::path&, const std::vector< std::string >& )
		-> result< pty_session > {
		return std::unexpected( fail( errc::unsupported,
			"pty_session::spawn is not implemented in M0 (docs/16 M1)" ) );
	}

	auto pty_session::write( const std::string_view ) -> status {
		return std::unexpected( fail( errc::unsupported, "pty_session::write is not implemented" ) );
	}

	auto pty_session::read( ) -> result< std::string > {
		return std::unexpected( fail( errc::unsupported, "pty_session::read is not implemented" ) );
	}

	auto pty_session::resize( const int, const int ) -> status {
		return std::unexpected( fail( errc::unsupported, "pty_session::resize is not implemented" ) );
	}

	auto pty_session::exit_code( ) const noexcept -> std::optional< int > {
		return std::nullopt;
	}

	auto sandbox_support_level( ) noexcept -> sandbox_support {
	#if defined( __linux__ )
		// Landlock + seccomp is the one implementation docs/16 M1 ships. It is not
		// wired up yet, so the honest answer is still `unavailable` -- callers must
		// fail closed rather than assume isolation.
		return sandbox_support::unavailable;
	#else
		return sandbox_support::unavailable;
	#endif
	}

	auto sandbox_mechanism( ) noexcept -> std::string_view {
	#if defined( _WIN32 )
		return "restricted-token + Job Object (not implemented in M0)";
	#elif defined( __APPLE__ )
		return "Seatbelt via sandbox_init_with_parameters (not implemented in M0)";
	#elif defined( __linux__ )
		return "Landlock + seccomp (not implemented in M0)";
	#else
		return "unknown platform";
	#endif
	}

	auto apply_sandbox( const sandbox_profile& ) -> status {
		return std::unexpected( fail( errc::unsupported,
			"sandbox enforcement is not implemented in M0; approvals fail closed instead "
			"(docs/16 M1)" ) );
	}

	auto terminate_process( const std::uint64_t process_id, const bool force ) -> status {
	#if defined( _WIN32 )
		const auto handle = OpenProcess( PROCESS_TERMINATE, FALSE,
			static_cast< DWORD >( process_id ) );

		if ( handle == nullptr ) {
			return std::unexpected( fail( errc::io, "OpenProcess failed" ) );
		}

		const auto flags = force ? static_cast< UINT >( 0 ) : static_cast< UINT >( 0 );
		const auto killed = TerminateProcess( handle, flags );
		CloseHandle( handle );

		if ( killed == 0 ) {
			return std::unexpected( fail( errc::io, "TerminateProcess failed" ) );
		}

		return { };
	#else
		// A graceful stop is SIGTERM; `force` is SIGKILL. On Windows there is no
		// graceful equivalent for an arbitrary process, which is why the flag is
		// accepted and ignored there rather than pretending.
		if ( ::kill( static_cast< pid_t >( process_id ), force ? SIGKILL : SIGTERM ) != 0 ) {
			return std::unexpected( fail( errc::io, "kill failed" ) );
		}

		return { };
	#endif
	}

	auto terminate_process_tree( const std::uint64_t process_id, const bool force ) -> status {
	#if defined( _WIN32 )
		// Killing the tree needs a Job Object handle, which is owned by whoever
		// spawned the child. Without it, this degrades to killing the root, and
		// saying so is better than implying the children died too.
		(void)process_id;
		(void)force;

		return std::unexpected( fail( errc::unsupported,
			"process-tree termination needs the spawn-time Job Object handle, "
			"which M0 does not yet thread through" ) );
	#else
		// A process group kill, which requires the child to have been spawned into
		// its own group.
		return terminate_process( process_id, force );
	#endif
	}

	auto process_is_alive( const std::uint64_t process_id ) -> bool {
	#if defined( _WIN32 )
		const auto handle = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
			static_cast< DWORD >( process_id ) );

		if ( handle == nullptr ) {
			return false;
		}

		auto code = DWORD{ 0 };
		const auto queried = GetExitCodeProcess( handle, &code );
		CloseHandle( handle );

		return queried != 0 && code == STILL_ACTIVE;
	#else
		return ::kill( static_cast< pid_t >( process_id ), 0 ) == 0;
	#endif
	}

	auto canonicalize( const std::filesystem::path& path ) -> result< std::filesystem::path > {
		auto error = std::error_code{ };
		auto resolved = std::filesystem::weakly_canonical( path, error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot canonicalize " + path.string( ) + ": " + error.message( ) ) );
		}

		return resolved;
	}

	auto temp_directory( ) -> result< std::filesystem::path > {
		auto error = std::error_code{ };
		auto path = std::filesystem::temp_directory_path( error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot resolve the temp directory: " + error.message( ) ) );
		}

		return path;
	}

	auto case_insensitive_paths( ) noexcept -> bool {
	#if defined( _WIN32 ) || defined( __APPLE__ )
		// macOS is case-insensitive by default on APFS, though a volume can be
		// formatted case-sensitive. The workspace boundary compares case-folded on
		// both, which is the conservative direction (docs/12).
		return true;
	#else
		return false;
	#endif
	}

	auto to_extended_path( const std::filesystem::path& path ) -> std::filesystem::path {
	#if defined( _WIN32 )
		auto text = path.wstring( );

		if ( text.starts_with( L"\\\\?\\" ) ) {
			return path;
		}

		if ( text.starts_with( L"\\\\" ) ) {
			// A UNC path takes the \\?\UNC\ form, not \\?\.
			return std::filesystem::path{ L"\\\\?\\UNC\\" + text.substr( 2 ) };
		}

		if ( text.size( ) >= 2 && text[ 1 ] == L':' ) {
			return std::filesystem::path{ L"\\\\?\\" + text };
		}

		return path;
	#else
		return path;
	#endif
	}

	auto app_data_path( const data_kind kind ) -> result< std::filesystem::path > {
		const auto name = std::string_view{ "mcode" };
		auto base = std::filesystem::path{ };
		auto suffix = std::string{ };

	#if defined( _WIN32 )
		switch ( kind ) {
			case data_kind::config: base = "APPDATA"; break;
			case data_kind::data: base = "LOCALAPPDATA"; break;
			case data_kind::cache: base = "LOCALAPPDATA"; break;
			case data_kind::state: base = "LOCALAPPDATA"; break;
			case data_kind::log: base = "LOCALAPPDATA"; break;
		}

		const auto* value = std::getenv( base.string( ).c_str( ) );

		if ( value == nullptr ) {
			return std::unexpected( fail( errc::io,
				"environment variable " + base.string( ) + " is not set" ) );
		}

		switch ( kind ) {
			case data_kind::cache: suffix = "Cache"; break;
			case data_kind::state: suffix = "State"; break;
			case data_kind::log: suffix = "Logs"; break;
			default: break;
		}

		auto out = std::filesystem::path{ value } / name;

		return suffix.empty( ) ? out : out / suffix;
	#else
		// XDG on Linux; macOS uses ~/Library/Application Support by convention.
		#if defined( __APPLE__ )
			const auto* home = std::getenv( "HOME" );

			if ( home == nullptr ) {
				return std::unexpected( fail( errc::io, "HOME is not set" ) );
			}

			auto out = std::filesystem::path{ home } / "Library" / "Application Support" / name;

			switch ( kind ) {
				case data_kind::cache: return std::filesystem::path{ home } / "Library" / "Caches" / name;
				case data_kind::log: return out / "Logs";
				case data_kind::state: return out / "State";
				default: return out;
			}
		#else
			const auto xdg = []( const char* variable, const char* fallback ) {
				const auto* value = std::getenv( variable );

				if ( value != nullptr && *value != '\0' ) {
					return std::filesystem::path{ value };
				}

				const auto* home = std::getenv( "HOME" );

				return std::filesystem::path{ home != nullptr ? home : "/tmp" } / fallback;
			};

			switch ( kind ) {
				case data_kind::config: return xdg( "XDG_CONFIG_HOME", ".config" ) / name;
				case data_kind::data: return xdg( "XDG_DATA_HOME", ".local/share" ) / name;
				case data_kind::cache: return xdg( "XDG_CACHE_HOME", ".cache" ) / name;
				case data_kind::state: return xdg( "XDG_STATE_HOME", ".local/state" ) / name;
				case data_kind::log: return xdg( "XDG_STATE_HOME", ".local/state" ) / name / "logs";
			}

			return std::unexpected( fail( errc::io, "unreachable data_kind" ) );
		#endif
	#endif
	}

	auto terminal_size( ) -> result< std::pair< int, int > > {
	#if defined( _WIN32 )
		auto info = CONSOLE_SCREEN_BUFFER_INFO{ };
		const auto handle = GetStdHandle( STD_OUTPUT_HANDLE );

		if ( handle == INVALID_HANDLE_VALUE || handle == nullptr ) {
			return std::unexpected( fail( errc::io, "no console handle" ) );
		}

		if ( GetConsoleScreenBufferInfo( handle, &info ) == 0 ) {
			return std::unexpected( fail( errc::io, "GetConsoleScreenBufferInfo failed" ) );
		}

		return std::pair{ static_cast< int >( info.srWindow.Right - info.srWindow.Left + 1 ),
			static_cast< int >( info.srWindow.Bottom - info.srWindow.Top + 1 ) };
	#else
		auto size = winsize{ };

		if ( ::ioctl( STDOUT_FILENO, TIOCGWINSZ, &size ) != 0 ) {
			return std::unexpected( fail( errc::io, "TIOCGWINSZ failed" ) );
		}

		return std::pair{ static_cast< int >( size.ws_col ), static_cast< int >( size.ws_row ) };
	#endif
	}

	auto terminal_size_changed( ) -> bool {
		// Polling rather than a callback keeps the loop single-threaded: the real
		// implementations are SIGWINCH, WINDOW_BUFFER_SIZE_EVENT, and in-band
		// DECSET 2048, and only the first is a signal.
		static auto last = terminal_size( );

		auto current = terminal_size( );

		if ( !current ) {
			return false;
		}

		if ( !last || *last != *current ) {
			last = current;

			return true;
		}

		return false;
	}

	auto file_watcher::supported( ) noexcept -> bool {
		// The interface exists so the extension API has a shape to target; the real
		// implementation is a Lua extension over a small core API (docs/24).
		return false;
	}

	auto file_watcher::create( const std::filesystem::path&, const bool ) -> result< file_watcher > {
		return std::unexpected( fail( errc::unsupported,
			"file watching is a Lua extension over the core API, not a core primitive (docs/24)" ) );
	}

	auto file_watcher::poll( ) -> std::vector< file_event > {
		return { };
	}

}
