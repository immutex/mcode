#include "mcode/platform/sandbox_windows.hxx"

#include <map>
#include <memory>
#include <string>
#include <utility>

#if defined( _WIN32 )
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#endif

namespace mcode::platform {

#if defined( _WIN32 )

	// A deterministic sandbox SID. One identity per machine keeps the ACL
	// boundary and any WFP filters addressable across runs without persisting
	// anything; it grants nothing by itself and is deny-only everywhere the
	// child looks.
	inline constexpr wchar_t SANDBOX_SID_STRING[] = L"S-1-5-21-3054197826-2915010853-3457101329-8173";

	[[nodiscard]] auto sandbox_windows_sid_string( ) -> std::wstring {
		return std::wstring{ SANDBOX_SID_STRING };
	}

	// Every limit the tier sets. Bounded by the same reasoning as the rest of
	// the file: the child is an untrusted command, so a quota is a guard, not a
	// policy knob.
	inline constexpr auto JOB_ACTIVE_PROCESS_LIMIT = std::uint32_t{ 32 };
	inline constexpr auto JOB_MEMORY_LIMIT_BYTES = std::uint64_t{ 2u * 1024u * 1024u * 1024u };

	// CloseHandle is not in the gate's list, but the header windows.h provides
	// it and the clang pass parses this file with _WIN32 defined; the POSIX leg
	// never compiles it. The deleter keeps every early error path leak-free.
	struct handle_deleter {
		auto operator( )( HANDLE value ) const -> void {
			if ( value != nullptr && value != INVALID_HANDLE_VALUE ) {
				::CloseHandle( value );
			}
		}
	};

	using unique_handle = std::unique_ptr< void, handle_deleter >;

	namespace {

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

	// A narrow error carries the Win32 reason. File-local: seams.cxx defines
	// its own copy because the two translation units must not share a symbol.
	[[nodiscard]] auto fail_win( const std::string& what, const DWORD error )
		-> mcode::error {
		return mcode::fail( mcode::errc::io,
			what + " failed: " + last_error_message( error ) );
	}

	// A well-formed DACL is built, not parsed. The deny ACE for the sandbox SID
	// comes first: canonical order puts explicit denies ahead of allows, and a
	// deny that sorts after an allow is not a deny.
	[[nodiscard]] auto build_boundary_descriptor( const PSID sandbox_sid )
		-> std::wstring {
		auto sid_text = LPWSTR{ nullptr };

		if ( !::ConvertSidToStringSidW( sandbox_sid, &sid_text ) || sid_text == nullptr ) {
			return { };
		}

		const auto sid = std::wstring{ sid_text };
		::LocalFree( sid_text );

		// Deny everything to the sandbox SID; the allow ACEs the caller grafts
		// onto write roots are separate descriptors that intersect with this
		// one. Everyone else keeps whatever they had.
		auto sddl = std::wstring{ L"D:" };
		sddl += L"(D;OICI;GA;;;" + sid + L")";

		return sddl;
	}
#endif

	}

	auto sandbox_windows_restricted_token( ) -> result< unique_token_windows > {
#if defined( _WIN32 )
		auto process_token = unique_handle{ nullptr };

		if ( !::OpenProcessToken( ::GetCurrentProcess( ), TOKEN_DUPLICATE | TOKEN_QUERY,
			reinterpret_cast< HANDLE* >( &process_token ) ) ) {
			return std::unexpected( fail_win( "OpenProcessToken", ::GetLastError( ) ) );
		}

		DWORD returned = 0;
		(void)::GetTokenInformation( process_token.get( ), TokenGroups, nullptr, 0, &returned );

		if ( returned == 0 ) {
			return std::unexpected( fail_win( "GetTokenInformation(TokenGroups) size",
				::GetLastError( ) ) );
		}

		auto groups_buffer = std::vector< unsigned char >( returned );
		const auto* groups = reinterpret_cast< const TOKEN_GROUPS* >( groups_buffer.data( ) );

		if ( !::GetTokenInformation( process_token.get( ), TokenGroups, groups_buffer.data( ),
			returned, &returned ) ) {
			return std::unexpected( fail_win( "GetTokenInformation(TokenGroups)",
				::GetLastError( ) ) );
		}

		// The logon SID identifies this logon session. Making it the sole
		// restricting SID is what makes the token restricted at all: a
		// restricted token with no restricting SID grants the intersection of
		// nothing with nothing, which is no restriction but reads like one.
		const SID_AND_ATTRIBUTES* logon_sid = nullptr;

		for ( DWORD index = 0; index < groups->GroupCount; ++index ) {
			if ( ( groups->Groups[ index ].Attributes & SE_GROUP_LOGON_ID ) != 0 ) {
				logon_sid = &groups->Groups[ index ];

				break;
			}
		}

		if ( logon_sid == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				"no logon SID in the process token; cannot restrict" ) );
		}

		// Every other group becomes deny-only. The user SID itself stays
		// enabled: the access check runs against the restricting SIDs, and
		// denying the user's own SID would break the profile's own allow ACEs.
		auto deny_buffer = std::vector< unsigned char >(
			sizeof( SID_AND_ATTRIBUTES ) * ( static_cast< std::size_t >( groups->GroupCount ) + 1 ) + 64 );

		auto* deny_groups = reinterpret_cast< TOKEN_GROUPS* >( deny_buffer.data( ) );
		deny_groups->GroupCount = 0;

		for ( DWORD index = 0; index < groups->GroupCount; ++index ) {
			const auto& source = groups->Groups[ index ];

			if ( ( source.Attributes & SE_GROUP_LOGON_ID ) != 0 ) {
				continue;
			}

			deny_groups->Groups[ deny_groups->GroupCount ].Sid = source.Sid;
			deny_groups->Groups[ deny_groups->GroupCount ].Attributes = SE_GROUP_USE_FOR_DENY_ONLY;
			++deny_groups->GroupCount;
		}

		auto restricted = unique_handle{ nullptr };

		auto restrict_sids = SID_AND_ATTRIBUTES{ };
		restrict_sids.Sid = logon_sid->Sid;
		restrict_sids.Attributes = 0;

		if ( !::CreateRestrictedToken( process_token.get( ), DISABLE_MAX_PRIVILEGE,
			deny_groups->GroupCount, deny_groups->Groups,
			0, nullptr,
			1, &restrict_sids,
			reinterpret_cast< PHANDLE >( &restricted ) ) ) {
			return std::unexpected( fail_win( "CreateRestrictedToken", ::GetLastError( ) ) );
		}

		return result< unique_token_windows >{ unique_token_windows{ restricted.release( ) } };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the restricted token exists only on Windows" ) );
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

	auto sandbox_windows_denied_everywhere( const PSID sandbox_sid )
		-> result< unique_descriptor_windows > {
#if defined( _WIN32 )
		if ( sandbox_sid == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				"no sandbox SID for the boundary descriptor" ) );
		}

		const auto sddl = build_boundary_descriptor( sandbox_sid );

		if ( sddl.empty( ) ) {
			return std::unexpected( fail_win( "ConvertSidToStringSidW", ::GetLastError( ) ) );
		}

		auto* descriptor = PSECURITY_DESCRIPTOR{ nullptr };

		if ( !::ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl.c_str( ), SDDL_REVISION_1, &descriptor, nullptr ) ) {
			return std::unexpected( fail_win( "ConvertStringSecurityDescriptorToSecurityDescriptor",
				::GetLastError( ) ) );
		}

		return unique_descriptor_windows{ descriptor };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the boundary descriptor exists only on Windows" ) );
#endif
	}

	auto sandbox_windows_grant_write( const std::filesystem::path& root,
		const void* sandbox_sid ) -> status {
#if defined( _WIN32 )
		if ( sandbox_sid == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				"no sandbox SID for the write grant" ) );
		}

		auto* sid = static_cast< PSID >( const_cast< void* >( sandbox_sid ) );

		auto* existing = PSECURITY_DESCRIPTOR{ nullptr };

		const auto query = ::GetNamedSecurityInfoW( root.c_str( ), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION, nullptr, nullptr,
			reinterpret_cast< PACL* >( &existing ), nullptr, &existing );

		if ( query != ERROR_SUCCESS || existing == nullptr ) {
			return std::unexpected( fail_win( "GetNamedSecurityInfoW", query ) );
		}

		auto guard = unique_descriptor_windows{ existing };

		ACL* dacl = nullptr;
		auto present = BOOL{ FALSE };
		auto defaulted = BOOL{ FALSE };

		if ( !::GetSecurityDescriptorDacl( guard.get( ), &present, &dacl, &defaulted ) ||
			!present || dacl == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				"the write root has no DACL to extend: " + root.string( ) ) );
		}

		auto info = ACL_SIZE_INFORMATION{ };

		if ( !::GetAclInformation( dacl, &info, sizeof( info ), AclSizeInformation ) ) {
			return std::unexpected( fail_win( "GetAclInformation", ::GetLastError( ) ) );
		}

		const auto ace_bytes = sizeof( ACCESS_ALLOWED_ACE ) - sizeof( DWORD ) +
			::GetLengthSid( sid );
		const auto new_size = info.AclBytesInUse + ace_bytes + sizeof( DWORD );

		auto new_dacl_buffer = std::vector< unsigned char >( new_size );

		if ( !::InitializeAcl( reinterpret_cast< ACL* >( new_dacl_buffer.data( ) ),
			static_cast< DWORD >( new_size ), ACL_REVISION_DS ) ) {
			return std::unexpected( fail_win( "InitializeAcl", ::GetLastError( ) ) );
		}

		auto* new_dacl = reinterpret_cast< ACL* >( new_dacl_buffer.data( ) );

		// The grant goes FIRST, ahead of the copied ACEs: the sandbox SID has no
		// ACE in the root's DACL, so order decides, and the allow must win on
		// the subtree the profile names.
		if ( !::AddAccessAllowedAceEx( new_dacl, ACL_REVISION_DS,
			CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
			GENERIC_WRITE | GENERIC_READ | GENERIC_EXECUTE | DELETE,
			sid ) ) {
			return std::unexpected( fail_win( "AddAccessAllowedAceEx", ::GetLastError( ) ) );
		}

		for ( DWORD ace_index = 0; ace_index < info.AceCount; ++ace_index ) {
			void* ace = nullptr;

			if ( !::GetAce( dacl, ace_index, &ace ) ) {
				return std::unexpected( fail_win( "GetAce", ::GetLastError( ) ) );
			}

			const auto* header = static_cast< const ACE_HEADER* >( ace );

			if ( !::AddAce( new_dacl, ACL_REVISION_DS, MAXDWORD,
				const_cast< LPVOID >( static_cast< const void* >( ace ) ),
				header->AceSize ) ) {
				return std::unexpected( fail_win( "AddAce", ::GetLastError( ) ) );
			}
		}

		// The descriptor was only the vehicle for the original DACL; the new one
		// is applied directly to the object. PROTECTED_DACL keeps the
		// inheritable deny from the parent out of the way of the grant.
		const auto set = ::SetNamedSecurityInfoW(
			const_cast< LPWSTR >( root.c_str( ) ), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
			nullptr, nullptr, new_dacl, nullptr );

		if ( set != ERROR_SUCCESS ) {
			return std::unexpected( fail_win( "SetNamedSecurityInfoW", set ) );
		}

		return { };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the write grant exists only on Windows" ) );
#endif
	}

}
