#include "mcode/platform/sandbox_windows.hxx"

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
			if ( const auto marked = mark_integrity_level( path, LOW_IL_SDDL ); !marked ) {
				return std::unexpected( marked.error( ) );
			}
		}

		for ( const auto& path : deny_paths ) {
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

}
