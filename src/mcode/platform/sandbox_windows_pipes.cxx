#include "mcode/platform/sandbox_windows.hxx"

#include "mcode/platform/sandbox_windows_internal.hxx"

#include <string>
#include <utility>

#if defined( _WIN32 )
#include <sddl.h>
#include <aclapi.h>
#endif

namespace mcode::platform {
#if defined( _WIN32 )

	auto sandbox_windows_close_pipes( sandbox_raw_pipes& pipes ) -> void {
		for ( auto* handle : { &pipes.child_stdin, &pipes.child_stdout, &pipes.child_stderr,
				 &pipes.parent_stdin, &pipes.parent_stdout, &pipes.parent_stderr } ) {
			if ( *handle != nullptr ) {
				::CloseHandle( static_cast< HANDLE >( *handle ) );
				*handle = nullptr;
			}
		}
	}

	auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes > {
		auto security = SECURITY_ATTRIBUTES{ };
		security.nLength = sizeof( security );
		security.bInheritHandle = TRUE;
		security.lpSecurityDescriptor = nullptr;

		// anonymous pipes cannot carry FILE_FLAG_OVERLAPPED; the IOCP service needs it.
		constexpr DWORD PIPE_BUFFER_BYTES = 0;

		// A fixed name collides with the previous spawn's still-open handles.
		static std::atomic< std::uint64_t > pipe_sequence{ 0 };
		const auto sequence = pipe_sequence.fetch_add( 1 );

		auto pipe_name = [ & ]( const wchar_t* suffix ) -> std::wstring {
			return std::wstring{ L"\\\\.\\pipe\\mcode-sandbox-" }
				+ std::to_wstring( ::GetCurrentProcessId( ) ) + L"-"
				+ std::to_wstring( sequence ) + suffix;
		};

		auto make_pair = [ & ]( const sandbox_pipe_direction direction,
			const std::wstring& name ) -> result< std::pair< HANDLE, HANDLE > > {
			const auto child_reads = ( direction == sandbox_pipe_direction::child_reads );

			// INBOUND grants the server GENERIC_READ, OUTBOUND grants it GENERIC_WRITE.
			const auto server_access = static_cast< DWORD >( child_reads
				? PIPE_ACCESS_INBOUND
				: PIPE_ACCESS_OUTBOUND );

			auto server = ::CreateNamedPipeW( name.c_str( ),
				server_access | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				DWORD{ 1 }, PIPE_BUFFER_BYTES, PIPE_BUFFER_BYTES, DWORD{ 0 },
				&security );

			if ( server == INVALID_HANDLE_VALUE ) {
				return std::unexpected( mcode::platform::detail::fail_win( "CreateNamedPipeW",
					::GetLastError( ) ) );
			}

			// the client takes the opposite access, so it must be the parent's end.
			const auto client_access = child_reads ? GENERIC_WRITE : GENERIC_READ;

			auto client = ::CreateFileW( name.c_str( ), client_access, 0, &security,
				OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr );

			if ( client == INVALID_HANDLE_VALUE ) {
				const auto error = ::GetLastError( );
				::CloseHandle( server );
				return std::unexpected( mcode::platform::detail::fail_win( "CreateFileW(pipe)", error ) );
			}

			// only the child's end is inheritable; an inherited parent end would outlive its close.
			if ( !::SetHandleInformation( server, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT ) ||
				!::SetHandleInformation( client, HANDLE_FLAG_INHERIT, 0 ) ) {
				const auto error = ::GetLastError( );
				::CloseHandle( server );
				::CloseHandle( client );
				return std::unexpected( mcode::platform::detail::fail_win( "SetHandleInformation", error ) );
			}

			return std::pair< HANDLE, HANDLE >{ server, client };
		};

		auto pipes = sandbox_raw_pipes{ };

		auto stdin_pair = make_pair( sandbox_pipe_direction::child_reads, pipe_name( L"-stdin" ) );

		if ( !stdin_pair ) {
			return std::unexpected( stdin_pair.error( ) );
		}

		pipes.child_stdin = stdin_pair->first;
		pipes.parent_stdin = stdin_pair->second;

		auto stdout_pair = make_pair( sandbox_pipe_direction::child_writes,
			pipe_name( L"-stdout" ) );

		if ( !stdout_pair ) {
			sandbox_windows_close_pipes( pipes );

			return std::unexpected( stdout_pair.error( ) );
		}

		pipes.child_stdout = stdout_pair->first;
		pipes.parent_stdout = stdout_pair->second;

		auto stderr_pair = make_pair( sandbox_pipe_direction::child_writes,
			pipe_name( L"-stderr" ) );

		if ( !stderr_pair ) {
			sandbox_windows_close_pipes( pipes );

			return std::unexpected( stderr_pair.error( ) );
		}

		pipes.child_stderr = stderr_pair->first;
		pipes.parent_stderr = stderr_pair->second;

		return pipes;
	}

#else

	auto sandbox_windows_close_pipes( sandbox_raw_pipes& ) -> void {
	}

	[[nodiscard]] auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes > {
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"pipes exist only on Windows" ) );
	}

#endif

}
