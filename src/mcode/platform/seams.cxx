#include "mcode/platform/seams.hxx"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

#if defined( __APPLE__ )
#include <mach-o/dyld.h>
#endif

#if defined( _WIN32 )
#include "mcode/platform/sandbox_windows.hxx"
#elif defined( __linux__ )
#include "mcode/platform/sandbox_linux.hxx"
#elif defined( __APPLE__ )
#include "mcode/platform/sandbox_macos.hxx"
#endif

namespace mcode::platform {


	// on Windows a pid is a DWORD, so a live pid above INT32_MAX is ordinary.
#if defined( _WIN32 )
	inline constexpr auto MAX_PROCESS_ID = std::uint32_t{ 0xFFFFFFFF };
#else
	inline constexpr auto MAX_PROCESS_ID = std::uint32_t{ 0x7FFFFFFF };
#endif

	// distinct from 0 so a supervisor can tell a killed child from a finished one.
	inline constexpr auto TERMINATED_EXIT_CODE = unsigned{ 1 };

	inline constexpr auto MAX_EXECUTABLE_PATH_BYTES = std::size_t{ 64u * 1024u };

	// to `kill`, 0 means the caller's whole process group.
	[[nodiscard]] auto is_plausible_process_id( const std::uint64_t process_id ) noexcept -> bool {
		return process_id != 0 && process_id <= static_cast< std::uint64_t >( MAX_PROCESS_ID );
	}

	auto pty_session::supported( ) noexcept -> bool {
		return false;
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

	auto sandbox_capability_level( ) noexcept -> sandbox_capability {
	#if defined( _WIN32 )
		// integrity levels have no read-down restriction, so reads are not confined.
		return sandbox_capability::write_boundary;
	#elif defined( __linux__ )
		return sandbox_capability::filesystem;
	#elif defined( __APPLE__ )
		return sandbox_capability::full;
	#else
		return sandbox_capability::unavailable;
	#endif
	}

	auto sandbox_network_level( ) noexcept -> sandbox_network_support {
	#if defined( _WIN32 )
		// A WFP egress deny needs admin; without it the network stays open.
		return sandbox_network_support::best_effort;
	#elif defined( __linux__ )
		const auto abi = sandbox_linux_abi( );

		if ( !abi || *abi < 1 ) {
			return sandbox_network_support::unavailable;
		}

		// ABI >= 4 denies at the Landlock layer; below that seccomp covers it.
		return sandbox_network_support::enforced;
	#elif defined( __APPLE__ )
		return sandbox_network_support::enforced;
	#else
		return sandbox_network_support::unavailable;
	#endif
	}

	auto sandbox_mechanism( ) noexcept -> std::string_view {
	#if defined( _WIN32 )
		return "Low IL token via CreateProcessAsUserW on the exec path; MCP server children get the Job Object only; network deny best-effort (WFP needs admin)";
	#elif defined( __APPLE__ )
		return "Seatbelt via sandbox_init_with_parameters, deny-default";
	#elif defined( __linux__ )
		return "Landlock (runtime ABI detected) + seccomp network fallback";
	#else
		return "unknown platform";
	#endif
	}

	auto apply_sandbox( const sandbox_profile& profile ) -> status {
#if defined( _WIN32 )
		// in-process only; the child path applies the Low IL token at spawn.
		if ( const auto marked = sandbox_windows_mark_write_paths(
			profile.write_paths, profile.deny_paths ); !marked ) {
			return std::unexpected( marked.error( ) );
		}

		return { };
#elif defined( __linux__ )
		const auto abi = sandbox_linux_abi( );

		if ( !abi ) {
			return std::unexpected( abi.error( ) );
		}

		if ( *abi < 1 ) {
			return std::unexpected( mcode::fail( mcode::errc::unsupported,
				"the kernel reports no usable Landlock ABI" ) );
		}

		auto ruleset = sandbox_linux_ruleset( *abi, profile );

		if ( !ruleset ) {
			return std::unexpected( ruleset.error( ) );
		}

		return sandbox_linux_restrict( *abi, *ruleset, profile );
#elif defined( __APPLE__ )
		const auto temp = temp_directory( );

		if ( !temp ) {
			return std::unexpected( temp.error( ) );
		}

		return sandbox_macos_init( profile, *temp );
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"no sandbox implementation for this platform" ) );
#endif
	}

	auto terminate_process( const std::uint64_t process_id, const bool force ) -> status {
		if ( !is_plausible_process_id( process_id ) ) {
			return std::unexpected( fail( errc::io,
				"invalid process id " + std::to_string( process_id ) ) );
		}

	#if defined( _WIN32 )
		const auto handle = OpenProcess( PROCESS_TERMINATE, FALSE,
			static_cast< DWORD >( process_id ) );

		if ( handle == nullptr ) {
			return std::unexpected( fail( errc::io, "OpenProcess failed" ) );
		}

		// Windows has no graceful stop; the flag is accepted and ignored.
		(void)force;

		const auto killed = TerminateProcess( handle, TERMINATED_EXIT_CODE );
		CloseHandle( handle );

		if ( killed == 0 ) {
			return std::unexpected( fail( errc::io, "TerminateProcess failed" ) );
		}

		return { };
	#else
		if ( ::kill( static_cast< pid_t >( process_id ), force ? SIGKILL : SIGTERM ) != 0 ) {
			return std::unexpected( fail( errc::io, "kill failed" ) );
		}

		return { };
	#endif
	}

	auto terminate_process_tree( const std::uint64_t process_id, const bool force ) -> status {
	#if defined( _WIN32 )
		(void)process_id;
		(void)force;

		return std::unexpected( fail( errc::unsupported,
			"process-tree termination needs the spawn-time Job Object handle, "
			"which M0 does not yet thread through" ) );
	#else
		(void)process_id;
		(void)force;

		return std::unexpected( fail( errc::unsupported,
			"process-tree termination needs the child spawned into its own group, "
			"which M0 does not yet do" ) );
	#endif
	}

	auto process_is_alive( const std::uint64_t process_id ) -> bool {
		if ( !is_plausible_process_id( process_id ) ) {
			return false;
		}

	#if defined( _WIN32 )
		// SYNCHRONIZE is required for the wait below; a query-only handle fails it.
		const auto handle = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
			static_cast< DWORD >( process_id ) );

		if ( handle == nullptr ) {
			return false;
		}

		// not GetExitCodeProcess: STILL_ACTIVE (259) is also a valid exit code.
		const auto waited = WaitForSingleObject( handle, 0 );
		CloseHandle( handle );

		return waited == WAIT_TIMEOUT;
	#else
		if ( ::kill( static_cast< pid_t >( process_id ), 0 ) == 0 ) {
			return true;
		}

		// EPERM means the process exists but is not ours to signal.
		return errno == EPERM;
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
		// APFS is case-insensitive by default; a volume may be formatted otherwise.
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

	// A truncated path silently becomes the wrong directory rather than an error.
	auto executable_directory( ) -> result< std::filesystem::path > {
	#if defined( _WIN32 )
		// GetModuleFileNameA truncates, returns the buffer size, and sets no error code.
		auto capacity = std::size_t{ MAX_PATH };
		auto buffer = std::vector< char >{ };

		for ( ;; ) {
			buffer.assign( capacity, '\0' );

			const auto written = ::GetModuleFileNameA( nullptr, buffer.data( ),
				static_cast< DWORD >( buffer.size( ) ) );

			if ( written == 0 ) {
				return std::unexpected( fail( errc::io, "GetModuleFileNameA failed" ) );
			}

			if ( written < buffer.size( ) ) {
				return std::filesystem::path{ std::string{ buffer.data( ), written } }
					.parent_path( );
			}

			if ( capacity >= MAX_EXECUTABLE_PATH_BYTES ) {
				return std::unexpected( fail( errc::io, "the executable path is too long" ) );
			}

			capacity *= 2;
		}
	#elif defined( __APPLE__ )
		// returns non-zero when the buffer was too small; the path may be relative.
		auto size = std::uint32_t{ 0 };

		(void)::_NSGetExecutablePath( nullptr, &size );

		if ( size == 0 ) {
			return std::unexpected( fail( errc::io, "_NSGetExecutablePath reported no size" ) );
		}

		auto buffer = std::vector< char >( size, '\0' );

		if ( ::_NSGetExecutablePath( buffer.data( ), &size ) != 0 ) {
			return std::unexpected( fail( errc::io, "_NSGetExecutablePath failed" ) );
		}

		auto error_code = std::error_code{ };
		auto resolved = std::filesystem::weakly_canonical(
			std::filesystem::path{ buffer.data( ) }, error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot resolve the executable path: " + error_code.message( ) ) );
		}

		return resolved.parent_path( );
	#else
		// readlink does not NUL-terminate and returns the length it would have written.
		auto capacity = std::size_t{ 256 };

		for ( ;; ) {
			auto buffer = std::vector< char >( capacity, '\0' );
			const auto written = ::readlink( "/proc/self/exe", buffer.data( ), buffer.size( ) );

			if ( written < 0 ) {
				return std::unexpected( fail( errc::io,
					"cannot read /proc/self/exe; the executable directory is unavailable" ) );
			}

			if ( static_cast< std::size_t >( written ) < buffer.size( ) ) {
				return std::filesystem::path{
					std::string{ buffer.data( ), static_cast< std::size_t >( written ) } }
					.parent_path( );
			}

			if ( capacity >= MAX_EXECUTABLE_PATH_BYTES ) {
				return std::unexpected( fail( errc::io, "the executable path is too long" ) );
			}

			capacity *= 2;
		}
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
		// polled to keep the loop single-threaded; only SIGWINCH is a signal.
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
}
