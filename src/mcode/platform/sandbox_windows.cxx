#include "mcode/platform/sandbox_windows.hxx"

#include <atomic>
#include <map>
#include <string>
#include <utility>

#if defined( _WIN32 )
#include <sddl.h>
#include <aclapi.h>
#endif

namespace mcode::platform {

#if defined( _WIN32 )

	// Every limit the tier sets. The child is an untrusted command, so a
	// quota is a guard, not a policy knob.
	inline constexpr auto JOB_ACTIVE_PROCESS_LIMIT = std::uint32_t{ 32 };
	inline constexpr auto JOB_MEMORY_LIMIT_BYTES = std::uint64_t{ 2u * 1024u * 1024u * 1024u };

	// The SDDL for a Low integrity label with the no-write-up policy, and for
	// a Medium label restoring the default on deny subtrees.
	inline constexpr wchar_t LOW_IL_SDDL[] = L"S:(ML;;NW;;;LW)";
	inline constexpr wchar_t MEDIUM_IL_SDDL[] = L"S:(ML;;NW;;;ME)";

	[[nodiscard]] auto last_error_message( const DWORD error ) -> std::string {
		auto* buffer = LPWSTR{ nullptr };

		const auto length = ::FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0,
			reinterpret_cast< LPWSTR >( &buffer ), 0, nullptr );

		if ( length == 0 || buffer == nullptr ) {
			return "windows error " + std::to_string( error );
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

		return utf8;
	}

	namespace {

		[[nodiscard]] auto fail_win( const std::string& what, const DWORD error )
			-> mcode::error {
			return mcode::fail( mcode::errc::io,
				what + " failed: " + last_error_message( error ) );
		}

	} 

#endif

	auto sandbox_windows_child_token( )
		-> result< unique_job_windows > {
#if defined( _WIN32 )
		auto process_token = unique_job_windows{ nullptr };

		if ( !::OpenProcessToken( ::GetCurrentProcess( ), TOKEN_DUPLICATE | TOKEN_QUERY,
			reinterpret_cast< HANDLE* >( &process_token ) ) ) {
			return std::unexpected( fail_win( "OpenProcessToken", ::GetLastError( ) ) );
		}

		auto primary = unique_job_windows{ nullptr };

		if ( !::DuplicateTokenEx( process_token.get( ), MAXIMUM_ALLOWED, nullptr,
			SecurityImpersonation, TokenPrimary,
			reinterpret_cast< HANDLE* >( &primary ) ) ) {
			return std::unexpected( fail_win( "DuplicateTokenEx", ::GetLastError( ) ) );
		}

		// Strip every privilege. SE_PRIVILEGE_REMOVED is the documented way to
		// delete rather than disable: a disabled privilege can be re-enabled,
		// a removed one cannot.
		DWORD returned = 0;
		(void)::GetTokenInformation( primary.get( ), TokenPrivileges, nullptr, 0, &returned );

		auto priv_buffer = std::vector< unsigned char >( returned );
		auto* privileges = reinterpret_cast< TOKEN_PRIVILEGES* >( priv_buffer.data( ) );

		if ( !::GetTokenInformation( primary.get( ), TokenPrivileges, priv_buffer.data( ),
			returned, &returned ) ) {
			return std::unexpected( fail_win( "GetTokenInformation(TokenPrivileges)",
				::GetLastError( ) ) );
		}

		for ( DWORD index = 0; index < privileges->PrivilegeCount; ++index ) {
			privileges->Privileges[ index ].Attributes = SE_PRIVILEGE_REMOVED;
		}

		if ( !::AdjustTokenPrivileges( primary.get( ), FALSE, privileges, returned,
			nullptr, nullptr ) ) {
			return std::unexpected( fail_win( "AdjustTokenPrivileges", ::GetLastError( ) ) );
		}

		// Low integrity. A Low IL process cannot write to a Medium IL object
		// (no write-up) but can read it, which is exactly the write boundary
		// this tier claims.
		SID_IDENTIFIER_AUTHORITY mandatory_authority = { { 0, 0, 0, 0, 0, 16 } };

		auto sid_buffer = std::vector< unsigned char >( SECURITY_MAX_SID_SIZE );
		auto* low_sid = reinterpret_cast< SID* >( sid_buffer.data( ) );

		if ( !::InitializeSid( low_sid, &mandatory_authority, 1 ) ) {
			return std::unexpected( fail_win( "InitializeSid", ::GetLastError( ) ) );
		}

		*::GetSidSubAuthority( low_sid, 0 ) = SECURITY_MANDATORY_LOW_RID;

		auto label = TOKEN_MANDATORY_LABEL{ };
		label.Label.Sid = low_sid;
		label.Label.Attributes = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;

		if ( !::SetTokenInformation( primary.get( ), TokenIntegrityLevel, &label,
			sizeof( label ) + ::GetLengthSid( low_sid ) ) ) {
			return std::unexpected( fail_win( "SetTokenInformation(TokenIntegrityLevel)",
				::GetLastError( ) ) );
		}

		return primary;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the child token exists only on Windows" ) );
#endif
	}

	auto sandbox_windows_job_create( ) -> result< unique_job_windows > {
#if defined( _WIN32 )
		auto job = unique_job_windows{ ::CreateJobObjectW( nullptr, nullptr ) };

		if ( job == nullptr ) {
			return std::unexpected( fail_win( "CreateJobObjectW", ::GetLastError( ) ) );
		}

		auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{ };

		limits.BasicLimitInformation.LimitFlags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
			JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
			JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		limits.BasicLimitInformation.ActiveProcessLimit = JOB_ACTIVE_PROCESS_LIMIT;
		limits.ProcessMemoryLimit = JOB_MEMORY_LIMIT_BYTES;

		if ( ::SetInformationJobObject( job.get( ), JobObjectExtendedLimitInformation,
			&limits, sizeof( limits ) ) == 0 ) {
			return std::unexpected( fail_win( "SetInformationJobObject", ::GetLastError( ) ) );
		}

		auto ui = JOBOBJECT_BASIC_UI_RESTRICTIONS{ };
		ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
			JOB_OBJECT_UILIMIT_READCLIPBOARD |
			JOB_OBJECT_UILIMIT_HANDLES |
			JOB_OBJECT_UILIMIT_GLOBALATOMS |
			JOB_OBJECT_UILIMIT_DISPLAYSETTINGS;

		if ( ::SetInformationJobObject( job.get( ), JobObjectBasicUIRestrictions,
			&ui, sizeof( ui ) ) == 0 ) {
			return std::unexpected( fail_win( "SetInformationJobObject(UI)", ::GetLastError( ) ) );
		}

		return job;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the Job Object exists only on Windows" ) );
#endif
	}

	namespace {

		// Applies one mandatory label to one path.
		[[nodiscard]] auto mark_integrity_level( const std::filesystem::path& path,
			const wchar_t* sddl ) -> status {
#if defined( _WIN32 )
			auto* descriptor = PSECURITY_DESCRIPTOR{ nullptr };

			if ( !::ConvertStringSecurityDescriptorToSecurityDescriptorW( sddl,
				SDDL_REVISION_1, &descriptor, nullptr ) ) {
				return std::unexpected( fail_win( "ConvertStringSecurityDescriptorToSecurityDescriptor",
					::GetLastError( ) ) );
			}

			auto guard = unique_job_windows{ descriptor };

			auto* sacl = PACL{ nullptr };
			auto present = BOOL{ FALSE };
			auto defaulted = BOOL{ FALSE };

			if ( !::GetSecurityDescriptorSacl( guard.get( ), &present, &sacl, &defaulted ) ||
				!present || sacl == nullptr ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"the label descriptor has no SACL: " + path.string( ) ) );
			}

			const auto set = ::SetNamedSecurityInfoW(
				const_cast< LPWSTR >( path.c_str( ) ), SE_FILE_OBJECT,
				LABEL_SECURITY_INFORMATION,
				nullptr, nullptr, nullptr, sacl );

			if ( set != ERROR_SUCCESS ) {
				return std::unexpected( fail_win( "SetNamedSecurityInfoW", set ) );
			}

			return { };
#else
			return std::unexpected( mcode::fail( mcode::errc::unsupported,
				"integrity labels exist only on Windows" ) );
#endif
		}

	}

	auto sandbox_windows_mark_write_paths(
		const std::vector< std::filesystem::path >& write_paths,
		const std::vector< std::filesystem::path >& deny_paths ) -> status {
#if defined( _WIN32 )
		for ( const auto& path : write_paths ) {
			if ( !std::filesystem::exists( path ) ) {
				continue;
			}

			if ( const auto marked = mark_integrity_level( path, LOW_IL_SDDL ); !marked ) {
				return std::unexpected( marked.error( ) );
			}
		}

		for ( const auto& path : deny_paths ) {
			if ( !std::filesystem::exists( path ) ) {
				// A protected path that does not exist cannot be labelled, and
				// a Low-IL child can therefore create it. That is a narrower
				// hole than failing every spawn: the engine still gates the
				// command, and a directory that does not exist has no contents
				// to corrupt.
				continue;
			}

			// Medium IL restores the default: a Low child is denied write-up,
			// which is what the deny means. Marking High would also deny
			// reads-by-registry tools; Medium denies exactly writes.
			if ( const auto marked = mark_integrity_level( path, MEDIUM_IL_SDDL ); !marked ) {
				return std::unexpected( marked.error( ) );
			}
		}

		return { };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"integrity labels exist only on Windows" ) );
#endif
	}

	auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes > {
#if defined( _WIN32 )
		auto security = SECURITY_ATTRIBUTES{ };
		security.nLength = sizeof( security );
		security.bInheritHandle = TRUE;
		security.lpSecurityDescriptor = nullptr;

		// Anonymous pipes cannot carry FILE_FLAG_OVERLAPPED, and the IOCP
		// handle service refuses a synchronous handle. Named pipes with the
		// overlapped flag on the parent ends are the documented equivalent.
		constexpr DWORD PIPE_BUFFER_BYTES = 0;

		// A per-call name: a fixed name collides with the previous spawn's
		// still-open handles, and "all pipe instances are busy" is the result.
		static std::atomic< std::uint64_t > pipe_sequence{ 0 };
		const auto sequence = pipe_sequence.fetch_add( 1 );

		auto pipe_name = [ & ]( const wchar_t* suffix ) -> std::wstring {
			return std::wstring{ L"\\\\.\\pipe\\mcode-sandbox-" }
				+ std::to_wstring( ::GetCurrentProcessId( ) ) + L"-"
				+ std::to_wstring( sequence ) + suffix;
		};

		auto make_pair = [ & ]( const bool child_end_inheritable,
			const std::wstring& name ) -> result< std::pair< HANDLE, HANDLE > > {
			const auto direction = child_end_inheritable
				? PIPE_ACCESS_INBOUND
				: PIPE_ACCESS_OUTBOUND;

			auto server = ::CreateNamedPipeW( name.c_str( ),
				direction | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				1, PIPE_BUFFER_BYTES, PIPE_BUFFER_BYTES, DWORD{ 0 }, &security );

			if ( server == INVALID_HANDLE_VALUE ) {
				return std::unexpected( fail_win( "CreateNamedPipeW",
					::GetLastError( ) ) );
			}

			const auto access = child_end_inheritable
				? GENERIC_WRITE
				: GENERIC_READ;

			auto client = ::CreateFileW( name.c_str( ), access, 0, &security,
				OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr );

			if ( client == INVALID_HANDLE_VALUE ) {
				const auto error = ::GetLastError( );
				::CloseHandle( server );
				return std::unexpected( fail_win( "CreateFileW(pipe)", error ) );
			}

			// The end the CHILD uses must be inheritable; the parent's end
			// must not be, so the child cannot wait on itself.
			const auto child_end = child_end_inheritable ? server : client;
			const auto parent_end = child_end_inheritable ? client : server;

			if ( !::SetHandleInformation( child_end, HANDLE_FLAG_INHERIT,
				HANDLE_FLAG_INHERIT ) ) {
				return std::unexpected( fail_win( "SetHandleInformation",
					::GetLastError( ) ) );
			}

			if ( !::SetHandleInformation( parent_end, HANDLE_FLAG_INHERIT, 0 ) ) {
				return std::unexpected( fail_win( "SetHandleInformation",
					::GetLastError( ) ) );
			}

			return std::pair< HANDLE, HANDLE >{ child_end, parent_end };
		};

		auto pipes = sandbox_raw_pipes{ };

		// stdin: child reads, parent writes.
		auto stdin_pair = make_pair( true, pipe_name( L"-stdin" ) );

		if ( !stdin_pair ) {
			return std::unexpected( stdin_pair.error( ) );
		}

		pipes.child_stdin = stdin_pair->first;
		pipes.parent_stdin = stdin_pair->second;

		// stdout: child writes, parent reads.
		auto stdout_pair = make_pair( false, pipe_name( L"-stdout" ) );

		if ( !stdout_pair ) {
			return std::unexpected( stdout_pair.error( ) );
		}

		pipes.child_stdout = stdout_pair->first;
		pipes.parent_stdout = stdout_pair->second;

		// stderr: child writes, parent reads.
		auto stderr_pair = make_pair( false, pipe_name( L"-stderr" ) );

		if ( !stderr_pair ) {
			return std::unexpected( stderr_pair.error( ) );
		}

		pipes.child_stderr = stderr_pair->first;
		pipes.parent_stderr = stderr_pair->second;

		return pipes;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the raw pipes exist only on Windows" ) );
#endif
	}

	auto sandbox_windows_spawn( const std::filesystem::path& executable,
		const std::vector< std::string >& arguments,
		const std::filesystem::path& working_directory,
		const std::map< std::string, std::string, std::less<> >& environment,
		const void* stdin_read, const void* stdout_write, const void* stderr_write,
		void* job, const void* token ) -> result< sandbox_spawn_windows > {
#if defined( _WIN32 )
		// The stdio handles the caller owns may have been consumed by an IOCP
		// association that altered their inheritability. Duplicate them for
		// the child so the child's copies are independently inheritable.
		auto duplicate_inheritable = []( const void* source ) -> HANDLE {
			auto duplicate = HANDLE{ nullptr };

			if ( ::DuplicateHandle( ::GetCurrentProcess( ),
				static_cast< HANDLE >( const_cast< void* >( source ) ),
				::GetCurrentProcess( ), &duplicate, 0, TRUE,
				DUPLICATE_SAME_ACCESS ) ) {
				return duplicate;
			}

			return static_cast< HANDLE >( const_cast< void* >( source ) );
		};

		auto child_stdin = duplicate_inheritable( stdin_read );
		auto child_stdout = duplicate_inheritable( stdout_write );
		auto child_stderr = duplicate_inheritable( stderr_write );
		// CreateProcessAsUserW does not search PATH the way CreateProcessW
		// does: a bare name must be resolved first, or the spawn fails with
		// "file not found" for a program that exists.
		auto resolved = executable;

		if ( resolved.is_relative( ) || resolved.parent_path( ).empty( ) ) {
			auto search = resolved.wstring( );
			auto found = std::vector< wchar_t >( MAX_PATH + 1, L'\0' );

			const auto length = ::SearchPathW( nullptr, search.c_str( ), L".exe",
				static_cast< DWORD >( found.size( ) ), found.data( ), nullptr );

			if ( length > 0 && length < found.size( ) ) {
				resolved = std::filesystem::path{ std::wstring{ found.data( ), length } };
			}
		}

		// The command line: quoted executable plus arguments quoted only when
		// they need it. cmd.exe's /c parsing strips the first and last quote
		// of the command it is given, so quoting every argument blindly turns
		// `cmd /c "a" "b c"` into a command named `a b c` -- quote-when-needed
		// keeps /c bare and the command string singly quoted, which is the
		// form cmd's own rules handle.
		auto quote = []( const std::wstring& value ) -> std::wstring {
			if ( value.find_first_of( L" \"" ) == std::wstring::npos ) {
				return value;
			}

			auto out = std::wstring{ L"\"" };
			for ( const auto character : value ) {
				if ( character == L'"' ) {
					out += L"\\\"";
				} else {
					out += character;
				}
			}

			out += L"\"";
			return out;
		};

		auto command_line = quote( resolved.wstring( ) );

		for ( const auto& argument : arguments ) {
			auto wide = std::wstring{ };
			const auto bytes = ::MultiByteToWideChar( CP_UTF8, 0, argument.c_str( ),
				static_cast< int >( argument.size( ) ), nullptr, 0 );
			wide.resize( static_cast< std::size_t >( bytes ) );
			::MultiByteToWideChar( CP_UTF8, 0, argument.c_str( ),
				static_cast< int >( argument.size( ) ), wide.data( ), bytes );

			command_line += L" ";
			command_line += quote( wide );
		}

		// The environment block: sorted, double-NUL terminated, UTF-16.
		auto sorted = std::map< std::wstring, std::wstring, std::less<> >{ };
		for ( const auto& [ key, value ] : environment ) {
			auto to_wide = []( const std::string& narrow ) -> std::wstring {
				const auto bytes = ::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str( ),
					static_cast< int >( narrow.size( ) ), nullptr, 0 );
				auto wide = std::wstring( static_cast< std::size_t >( bytes ), L'\0' );
				::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str( ),
					static_cast< int >( narrow.size( ) ), wide.data( ), bytes );
				return wide;
			};

			sorted.emplace( to_wide( key ), to_wide( value ) );
		}

		auto env_block = std::wstring{ };
		for ( const auto& [ key, value ] : sorted ) {
			env_block += key;
			env_block += L'=';
			env_block += value;
			env_block += L'\0';
		}

		// The block is double-NUL terminated: one for the last entry, one for
		// the end of the block.
		env_block += L'\0';

		// Inheritable stdio: the three handles must be inheritable, which the
		// pipe creation in the caller already arranged via SECURITY_ATTRIBUTES.
		STARTUPINFOEXW startup_info{ };
		startup_info.StartupInfo.cb = sizeof( startup_info );
		startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup_info.StartupInfo.hStdInput = child_stdin;
		startup_info.StartupInfo.hStdOutput = child_stdout;
		startup_info.StartupInfo.hStdError = child_stderr;

		auto size = SIZE_T{ 0 };
		(void)::InitializeProcThreadAttributeList( nullptr, 1, 0, &size );

		auto storage = std::vector< unsigned char >( size );
		startup_info.lpAttributeList = reinterpret_cast< LPPROC_THREAD_ATTRIBUTE_LIST >(
			storage.data( ) );

		if ( !::InitializeProcThreadAttributeList( startup_info.lpAttributeList, 1, 0, &size ) ) {
			return std::unexpected( fail_win( "InitializeProcThreadAttributeList",
				::GetLastError( ) ) );
		}

		if ( !::UpdateProcThreadAttribute( startup_info.lpAttributeList, 0,
			static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER ) |
				static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_INPUT_FLAG ),
			&job, sizeof( job ), nullptr, nullptr ) ) {
			return std::unexpected( fail_win( "UpdateProcThreadAttribute", ::GetLastError( ) ) );
		}

		auto process_information = PROCESS_INFORMATION{ };

		const auto created = ::CreateProcessAsUserW(
			static_cast< HANDLE >( const_cast< void* >( token ) ),
			resolved.empty( ) ? nullptr : resolved.c_str( ),
			command_line.data( ),
			nullptr, nullptr,
			TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			env_block.data( ),
			working_directory.empty( ) ? nullptr : working_directory.c_str( ),
			&startup_info.StartupInfo,
			&process_information );

		::DeleteProcThreadAttributeList( startup_info.lpAttributeList );

		::CloseHandle( child_stdin );
		::CloseHandle( child_stdout );
		::CloseHandle( child_stderr );

		if ( created == 0 ) {
			return std::unexpected( fail_win( "CreateProcessAsUserW", ::GetLastError( ) ) );
		}

		::CloseHandle( process_information.hThread );

		return sandbox_spawn_windows{ process_information.dwProcessId,
			process_information.hProcess };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the raw spawn exists only on Windows" ) );
#endif
	}

}
