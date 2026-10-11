#include "mcode/platform/sandbox_windows.hxx"

#include "mcode/platform/sandbox_windows_internal.hxx"

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

	namespace {

		inline constexpr auto JOB_ACTIVE_PROCESS_LIMIT = std::uint32_t{ 32 };
		inline constexpr auto JOB_MEMORY_LIMIT_BYTES = std::uint64_t{ 2u * 1024u * 1024u * 1024u };

		// low IL with no-write-up, and Medium IL restoring the default on deny subtrees.
		inline constexpr wchar_t LOW_IL_SDDL[] = L"S:(ML;;NW;;;LW)";
		inline constexpr wchar_t MEDIUM_IL_SDDL[] = L"S:(ML;;NW;;;ME)";

		// CRT rule: space or tab delimits, and backslashes before a quote double.
		[[nodiscard]] auto quote_command_line_argument( const std::wstring& value )
			-> std::wstring {
			if ( !value.empty( ) && value.find_first_of( L" \t\"" ) == std::wstring::npos ) {
				return value;
			}

			auto quoted = std::wstring{ };
			quoted.reserve( value.size( ) + 2 );
			quoted += L'"';

			auto backslashes = std::size_t{ 0 };

			for ( const auto character : value ) {
				if ( character == L'\\' ) {
					++backslashes;

					continue;
				}

				if ( character == L'"' ) {
					// 2n+1 so the last one escapes the quote instead of closing the argument.
					quoted.append( backslashes * 2 + 1, L'\\' );
					backslashes = 0;
					quoted += L'"';

					continue;
				}

				quoted.append( backslashes, L'\\' );
				backslashes = 0;
				quoted += character;
			}

			quoted.append( backslashes * 2, L'\\' );
			quoted += L'"';

			return quoted;
		}

		[[nodiscard]] auto to_wide_string( const std::string& narrow ) -> std::wstring {
			const auto bytes = ::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str( ),
				static_cast< int >( narrow.size( ) ), nullptr, 0 );
			auto wide = std::wstring( static_cast< std::size_t >( bytes ), L'\0' );
			::MultiByteToWideChar( CP_UTF8, 0, narrow.c_str( ),
				static_cast< int >( narrow.size( ) ), wide.data( ), bytes );

			return wide;
		}

	}

#endif

	auto sandbox_windows_child_token( )
		-> result< unique_handle_windows > {
#if defined( _WIN32 )
		auto process_token = unique_handle_windows{ nullptr };

		if ( !::OpenProcessToken( ::GetCurrentProcess( ), TOKEN_DUPLICATE | TOKEN_QUERY,
			reinterpret_cast< HANDLE* >( &process_token ) ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "OpenProcessToken", ::GetLastError( ) ) );
		}

		auto primary = unique_handle_windows{ nullptr };

		if ( !::DuplicateTokenEx( process_token.get( ), MAXIMUM_ALLOWED, nullptr,
			SecurityImpersonation, TokenPrimary,
			reinterpret_cast< HANDLE* >( &primary ) ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "DuplicateTokenEx", ::GetLastError( ) ) );
		}

		// SE_PRIVILEGE_REMOVED deletes rather than disables.
		DWORD returned = 0;
		(void)::GetTokenInformation( primary.get( ), TokenPrivileges, nullptr, 0, &returned );

		auto priv_buffer = std::vector< unsigned char >( returned );
		auto* privileges = reinterpret_cast< TOKEN_PRIVILEGES* >( priv_buffer.data( ) );

		if ( !::GetTokenInformation( primary.get( ), TokenPrivileges, priv_buffer.data( ),
			returned, &returned ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "GetTokenInformation(TokenPrivileges)",
				::GetLastError( ) ) );
		}

		for ( DWORD index = 0; index < privileges->PrivilegeCount; ++index ) {
			privileges->Privileges[ index ].Attributes = SE_PRIVILEGE_REMOVED;
		}

		if ( !::AdjustTokenPrivileges( primary.get( ), FALSE, privileges, returned,
			nullptr, nullptr ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "AdjustTokenPrivileges", ::GetLastError( ) ) );
		}

		SID_IDENTIFIER_AUTHORITY mandatory_authority = { { 0, 0, 0, 0, 0, 16 } };

		auto sid_buffer = std::vector< unsigned char >( SECURITY_MAX_SID_SIZE );
		auto* low_sid = reinterpret_cast< SID* >( sid_buffer.data( ) );

		if ( !::InitializeSid( low_sid, &mandatory_authority, 1 ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "InitializeSid", ::GetLastError( ) ) );
		}

		*::GetSidSubAuthority( low_sid, 0 ) = SECURITY_MANDATORY_LOW_RID;

		auto label = TOKEN_MANDATORY_LABEL{ };
		label.Label.Sid = low_sid;
		label.Label.Attributes = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;

		if ( !::SetTokenInformation( primary.get( ), TokenIntegrityLevel, &label,
			sizeof( label ) + ::GetLengthSid( low_sid ) ) ) {
			return std::unexpected( mcode::platform::detail::fail_win( "SetTokenInformation(TokenIntegrityLevel)",
				::GetLastError( ) ) );
		}

		return primary;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the child token exists only on Windows" ) );
#endif
	}

	auto sandbox_windows_job_create( ) -> result< unique_handle_windows > {
#if defined( _WIN32 )
		auto job = unique_handle_windows{ ::CreateJobObjectW( nullptr, nullptr ) };

		if ( job == nullptr ) {
			return std::unexpected( mcode::platform::detail::fail_win( "CreateJobObjectW", ::GetLastError( ) ) );
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
			return std::unexpected( mcode::platform::detail::fail_win( "SetInformationJobObject", ::GetLastError( ) ) );
		}

		auto ui = JOBOBJECT_BASIC_UI_RESTRICTIONS{ };
		ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
			JOB_OBJECT_UILIMIT_READCLIPBOARD |
			JOB_OBJECT_UILIMIT_HANDLES |
			JOB_OBJECT_UILIMIT_GLOBALATOMS |
			JOB_OBJECT_UILIMIT_DISPLAYSETTINGS;

		if ( ::SetInformationJobObject( job.get( ), JobObjectBasicUIRestrictions,
			&ui, sizeof( ui ) ) == 0 ) {
			return std::unexpected( mcode::platform::detail::fail_win( "SetInformationJobObject(UI)", ::GetLastError( ) ) );
		}

		return job;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the Job Object exists only on Windows" ) );
#endif
	}

#if defined( _WIN32 )

	namespace {

		[[nodiscard]] auto mark_integrity_level( const std::filesystem::path& path,
			const wchar_t* sddl ) -> status {
			auto* descriptor = PSECURITY_DESCRIPTOR{ nullptr };

			if ( !::ConvertStringSecurityDescriptorToSecurityDescriptorW( sddl,
				SDDL_REVISION_1, &descriptor, nullptr ) ) {
				return std::unexpected(
					mcode::platform::detail::fail_win( "ConvertStringSecurityDescriptorToSecurityDescriptor",
						::GetLastError( ) ) );
			}

			auto guard = unique_handle_windows{ descriptor };

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
				return std::unexpected( mcode::platform::detail::fail_win( "SetNamedSecurityInfoW", set ) );
			}

			return { };
		}

	}

#endif

	auto sandbox_windows_mark_write_paths(
		const std::vector< std::filesystem::path >& write_paths,
		const std::vector< std::filesystem::path >& deny_paths ) -> status {
#if !defined( _WIN32 )
		(void)write_paths;
		(void)deny_paths;
#endif

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
				// a missing deny path cannot be labelled, so a Low-IL child can create it.
				continue;
			}

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

	auto sandbox_windows_spawn( const std::filesystem::path& executable,
		const std::vector< std::string >& arguments,
		const std::filesystem::path& working_directory,
		const std::map< std::string, std::string, std::less<> >& environment,
		const sandbox_spawn_windows_handles& handles ) -> result< sandbox_spawn_windows > {
		const auto stdin_read = handles.stdin_read;
		const auto stdout_write = handles.stdout_write;
		const auto stderr_write = handles.stderr_write;

		// Not `const`: `UpdateProcThreadAttribute` takes the address of the handle,
		// and `&` on a `void* const` is a `void* const*`.
		auto job = handles.job;

		const auto token = handles.token;
#if !defined( _WIN32 )
		(void)executable;
		(void)arguments;
		(void)working_directory;
		(void)environment;
		(void)stdin_read;
		(void)stdout_write;
		(void)stderr_write;
		(void)job;
		(void)token;
#endif

#if defined( _WIN32 )
		// an IOCP association may have altered the caller's handles, so the child gets copies.
		auto duplicate_inheritable = []( const void* source ) -> result< HANDLE > {
			auto duplicate = HANDLE{ nullptr };

			if ( !::DuplicateHandle( ::GetCurrentProcess( ),
				static_cast< HANDLE >( const_cast< void* >( source ) ),
				::GetCurrentProcess( ), &duplicate, 0, TRUE,
				DUPLICATE_SAME_ACCESS ) ) {
				return std::unexpected( mcode::platform::detail::fail_win( "DuplicateHandle", ::GetLastError( ) ) );
			}

			return duplicate;
		};

		auto child_stdin = duplicate_inheritable( stdin_read );

		if ( !child_stdin ) {
			return std::unexpected( child_stdin.error( ) );
		}

		auto child_stdout = duplicate_inheritable( stdout_write );

		if ( !child_stdout ) {
			::CloseHandle( *child_stdin );

			return std::unexpected( child_stdout.error( ) );
		}

		auto child_stderr = duplicate_inheritable( stderr_write );

		if ( !child_stderr ) {
			::CloseHandle( *child_stdin );
			::CloseHandle( *child_stdout );

			return std::unexpected( child_stderr.error( ) );
		}

		// every exit below has to release the three duplicates and the attribute list.
		auto close_children = [ & ]( ) {
			::CloseHandle( *child_stdin );
			::CloseHandle( *child_stdout );
			::CloseHandle( *child_stderr );
		};

		// CreateProcessAsUserW does not search PATH the way CreateProcessW does.
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

		// quote only when needed: cmd.exe /c strips the first and last quote it is given.
		auto command_line = quote_command_line_argument( resolved.wstring( ) );

		for ( const auto& argument : arguments ) {
			command_line += L" ";
			command_line += quote_command_line_argument( to_wide_string( argument ) );
		}

		auto sorted = std::map< std::wstring, std::wstring, std::less<> >{ };
		for ( const auto& [ key, value ] : environment ) {
			sorted.emplace( to_wide_string( key ), to_wide_string( value ) );
		}

		auto env_block = std::wstring{ };
		for ( const auto& [ key, value ] : sorted ) {
			env_block += key;
			env_block += L'=';
			env_block += value;
			env_block += L'\0';
		}

		env_block += L'\0';

		STARTUPINFOEXW startup_info{ };
		startup_info.StartupInfo.cb = sizeof( startup_info );
		startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup_info.StartupInfo.hStdInput = *child_stdin;
		startup_info.StartupInfo.hStdOutput = *child_stdout;
		startup_info.StartupInfo.hStdError = *child_stderr;

		auto size = SIZE_T{ 0 };
		(void)::InitializeProcThreadAttributeList( nullptr, 1, 0, &size );

		auto storage = std::vector< unsigned char >( size );
		startup_info.lpAttributeList = reinterpret_cast< LPPROC_THREAD_ATTRIBUTE_LIST >(
			storage.data( ) );

		if ( !::InitializeProcThreadAttributeList( startup_info.lpAttributeList, 1, 0, &size ) ) {
			const auto error = ::GetLastError( );
			close_children( );

			return std::unexpected( mcode::platform::detail::fail_win( "InitializeProcThreadAttributeList", error ) );
		}

		if ( !::UpdateProcThreadAttribute( startup_info.lpAttributeList, 0,
			static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER ) |
				static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_INPUT_FLAG ),
			&job, sizeof( job ), nullptr, nullptr ) ) {
			const auto error = ::GetLastError( );
			::DeleteProcThreadAttributeList( startup_info.lpAttributeList );
			close_children( );

			return std::unexpected( mcode::platform::detail::fail_win( "UpdateProcThreadAttribute", error ) );
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

		close_children( );

		if ( created == 0 ) {
			return std::unexpected( mcode::platform::detail::fail_win( "CreateProcessAsUserW", ::GetLastError( ) ) );
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
