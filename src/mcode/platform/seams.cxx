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
#include <sddl.h>
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

#if defined( _WIN32 )
	// A narrow error carries the Win32 reason. Defined here because the
	// apply_sandbox branch in this file names the failure the same way the
	// sandbox_windows.cxx helpers do.
	[[nodiscard]] auto fail_win( const std::string& what, const DWORD error )
		-> mcode::error {
		auto* buffer = LPWSTR{ nullptr };

		const auto length = ::FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0,
			reinterpret_cast< LPWSTR >( &buffer ), 0, nullptr );

		if ( length == 0 || buffer == nullptr ) {
			return fail( errc::io, what + " failed: windows error " +
				std::to_string( error ) );
		}

		const auto text = std::wstring{ buffer, length };
		::LocalFree( buffer );

		auto utf8 = std::string{ };
		const auto bytes = ::WideCharToMultiByte( CP_UTF8, 0, text.c_str( ),
			static_cast< int >( text.size( ) ), nullptr, 0, nullptr, nullptr );

		if ( bytes > 0 ) {
			utf8.resize( static_cast< std::size_t >( bytes ) );
			::WideCharToMultiByte( CP_UTF8, 0, text.c_str( ),
				static_cast< int >( text.size( ) ), utf8.data( ), bytes, nullptr, nullptr );
		}

		while ( !utf8.empty( ) && ( utf8.back( ) == '\r' || utf8.back( ) == '\n' ) ) {
			utf8.pop_back( );
		}

		return fail( errc::io, what + " failed: " + utf8 );
	}
#endif

	// The largest plausible process id on this platform.
	//
	// On POSIX it is pid_t's positive range, because `kill` reinterprets anything
	// else as a signal target. On Windows a pid is a DWORD and the allocator uses
	// the whole unsigned range, so after long uptime a live pid above 2^31 is
	// ordinary -- bounding it at INT32_MAX would report a running process as dead.
#if defined( _WIN32 )
	inline constexpr auto MAX_PROCESS_ID = std::uint32_t{ 0xFFFFFFFF };
#else
	inline constexpr auto MAX_PROCESS_ID = std::uint32_t{ 0x7FFFFFFF };
#endif

	// What a killed child exits with. Distinct from 0 so a supervisor can tell a
	// process we terminated from one that finished on its own.
	//
	// `unsigned` and not `UINT`: that typedef is Win32-only, and this constant is
	// declared outside the platform guard.
	inline constexpr auto TERMINATED_EXIT_CODE = unsigned{ 1 };

	// A path longer than this is not a usable extension root on any platform, and
	// the lookup loops double their buffer, so the cap is what stops a pathological
	// case from growing without bound.
	inline constexpr auto MAX_EXECUTABLE_PATH_BYTES = std::size_t{ 64u * 1024u };

	// Zero and above-max are both not process ids. Zero is the dangerous one: to
	// `kill` it means the caller's whole process group.
	[[nodiscard]] auto is_plausible_process_id( const std::uint64_t process_id ) noexcept -> bool {
		return process_id != 0 && process_id <= static_cast< std::uint64_t >( MAX_PROCESS_ID );
	}

	// M0 ships interfaces and honest stubs. Where a platform cannot do the thing
	// yet, the call returns `unsupported` and `sandbox_capability_level` says
	// so -- a stub that silently succeeded would let a caller believe it was
	// isolated.

	auto pty_session::supported( ) noexcept -> bool {
		// False until spawn exists. The predicate exists so a caller can decide
		// whether to use the PTY path at all, and returning true while every
		// operation returns `unsupported` converts a design-time signal into a
		// runtime surprise -- exactly the dishonesty this file's rule forbids.
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
		// Tier 1 needs no administrator: restricted token, Job Object and the
		// ACL boundary all work under a normal user token. Egress is NOT
		// claimed here -- sandbox_network_level says best_effort, and `full`
		// would assert a network denial this tier cannot deliver without admin.
		return sandbox_capability::filesystem;
	#elif defined( __linux__ )
		// Landlock's ABI is a runtime property of the kernel, so this constant
		// is a platform-capability claim, not a runtime one: a kernel without
		// Landlock hard-fails in apply_sandbox and the spawn path reports that
		// refusal. sandbox_linux_abi( ) is the runtime probe.
		return sandbox_capability::filesystem;
	#elif defined( __APPLE__ )
		// Seatbelt applies in-process before the child's first exec, so the
		// filesystem tier is enforced whenever the profile applies.
		return sandbox_capability::full;
	#else
		return sandbox_capability::unavailable;
	#endif
	}

	auto sandbox_network_level( ) noexcept -> sandbox_network_support {
	#if defined( _WIN32 )
		// The filesystem tier needs no admin; a WFP deny does. Without admin
		// the network stays open while the filesystem is confined, which is
		// exactly why this is a separate enum: "full" would be a false claim.
		return sandbox_network_support::best_effort;
	#elif defined( __linux__ )
		// ABI >= 4 denies at the Landlock layer; below that the seccomp filter
		// covers it. Both are real denials, so the answer is enforced either
		// way -- the mechanism string names which one applied.
		return sandbox_network_support::enforced;
	#elif defined( __APPLE__ )
		return sandbox_network_support::enforced;
	#else
		return sandbox_network_support::unavailable;
	#endif
	}

	auto sandbox_mechanism( ) noexcept -> std::string_view {
	#if defined( _WIN32 )
		return "restricted-token + Job Object + ACL write boundary; network deny best-effort (WFP needs admin)";
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
		// The SID comes from the fixed string the whole tier shares.
		auto sid_text = std::wstring{ sandbox_windows_sid_string( ) };
		PSID sandbox_sid = nullptr;

		if ( !::ConvertStringSidToSidW( sid_text.c_str( ), &sandbox_sid ) ) {
			return std::unexpected( fail_win( "ConvertStringSidToSidW", ::GetLastError( ) ) );
		}

		auto sid_owner = std::unique_ptr< void, decltype( &::LocalFree ) >{
			sandbox_sid, &::LocalFree };

		for ( const auto& root : profile.write_paths ) {
			if ( const auto granted = sandbox_windows_grant_write( root, sandbox_sid ); !granted ) {
				return std::unexpected( granted.error( ) );
			}
		}

		// This entry point restricts the CALLING process; the child path goes
		// through the launcher in proc/, which builds the token and Job per
		// spawn. Restricting the harness here would confine the wrong process,
		// so the caller opts in explicitly by calling this with a profile it
		// means for itself.
		return { };
#elif defined( __linux__ )
		const auto abi = sandbox_linux_abi( );

		if ( !abi ) {
			return std::unexpected( abi.error( ) );
		}

		// A newer ABI is a superset and is handled by capping the right set at
		// LANDLOCK_ABI_MAX. An ABI the ladder does not cover at all cannot
		// happen above the max on a released kernel, and the ruleset builder
		// refuses anything below 1.
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
		// The same guard process_is_alive carries, for a stronger reason: kill(0)
		// signals the CALLER'S process group, so an unvalidated zero would deliver
		// SIGTERM to mcode itself and everything beside it, and report success.
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

		// `force` has no Windows equivalent: TerminateProcess is already
		// unconditional, so the flag is accepted and ignored rather than pretending
		// to select between a graceful and a hard stop. The exit code is distinct
		// from 0 so a supervisor can tell a killed child from one that finished.
		(void)force;

		const auto killed = TerminateProcess( handle, TERMINATED_EXIT_CODE );
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
		//
		// The parameters are unnamed in the POSIX branch below only because that
		// branch does not use them; they stay named here for the signature.
		(void)process_id;
		(void)force;

		return std::unexpected( fail( errc::unsupported,
			"process-tree termination needs the spawn-time Job Object handle, "
			"which M0 does not yet thread through" ) );
	#else
		// Only the root, because a group kill needs the child to have been spawned
		// into its own group and nothing spawns one yet. Saying `unsupported` is
		// honest; killing the root and letting the caller believe the tree died is
		// the failure the file's own rule names.
		(void)process_id;
		(void)force;

		return std::unexpected( fail( errc::unsupported,
			"process-tree termination needs the child spawned into its own group, "
			"which M0 does not yet do" ) );
	#endif
	}

	auto process_is_alive( const std::uint64_t process_id ) -> bool {
		// Zero and -1 are not pids to `kill`: 0 means the caller's process group and
		// -1 means every process the caller may signal, and both SUCCEED. So a
		// truncated 0xFFFFFFFF would report as alive, and a caller passing a
		// pid-sized value to a kill call would broadcast. Validate before the
		// syscall.
		if ( !is_plausible_process_id( process_id ) ) {
			return false;
		}

	#if defined( _WIN32 )
		// SYNCHRONIZE is required for WaitForSingleObject below; a query-only handle
		// makes the wait fail, which would report every live process as dead.
		const auto handle = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
			static_cast< DWORD >( process_id ) );

		if ( handle == nullptr ) {
			return false;
		}

		// A zero-timeout wait, not the exit code. GetExitCodeProcess returning
		// STILL_ACTIVE is the classic false-alive: a process that terminated with
		// exit code 259 reports as alive forever.
		const auto waited = WaitForSingleObject( handle, 0 );
		CloseHandle( handle );

		return waited == WAIT_TIMEOUT;
	#else
		if ( ::kill( static_cast< pid_t >( process_id ), 0 ) == 0 ) {
			return true;
		}

		// EPERM means the process EXISTS but is not ours to signal. Reporting it as
		// dead would make a supervisor reap a child that is still running.
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
		// macOS is case-insensitive by default on APFS, though a volume can be
		// formatted case-sensitive. The workspace boundary compares case-folded on
		// both, which is the conservative direction.
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

	// The directory the running binary sits in.
	//
	// Three different mechanisms, and the buffer handling matters more than the
	// lookup: every one of them reports "too long" differently, and a partial path
	// silently becomes the wrong directory rather than an error.
	auto executable_directory( ) -> result< std::filesystem::path > {
	#if defined( _WIN32 )
		// GetModuleFileNameA truncates and returns the buffer size, with no error
		// code and a NUL-terminated string, so a full buffer is the only signal
		// that the path did not fit. Grow and retry rather than accept a prefix.
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
		// _NSGetExecutablePath reports the required size in its own argument and
		// returns non-zero when the buffer was too small, so the second call is the
		// real one. The path it writes may be relative or contain symlinks.
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
		// /proc/self/exe is a symlink to the binary; readlink does not NUL-terminate
		// and returns the length it would have written, so a full buffer means the
		// result was truncated.
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
		// implementation is a Lua extension over a small core API.
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
