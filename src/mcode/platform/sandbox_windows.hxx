#pragma once

// Windows half of the Sandbox seam: restricted token, Job Object, and the ACL
// write boundary. The types are owned here so the launcher can hold them for
// the child's whole lifetime and the header stays out of portable code.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <boost/system/error_code.hpp>
#include <boost/process/v2/default_launcher.hpp>

#include "mcode/core/error.hxx"

#if defined( _WIN32 )
#include <windows.h>
#include <processthreadsapi.h>
#endif

namespace mcode::platform {

#if defined( _WIN32 )

	struct handle_closer {
		auto operator( )( void* value ) const -> void {
			if ( value != nullptr && value != INVALID_HANDLE_VALUE ) {
				::CloseHandle( static_cast< HANDLE >( value ) );
			}
		}
	};

	struct descriptor_closer {
		auto operator( )( void* value ) const -> void {
			if ( value != nullptr ) {
				::LocalFree( static_cast< HLOCAL >( value ) );
			}
		}
	};

	// The restricted child token. A restricted token without a restricting SID
	// is not a restriction, so construction refuses rather than returning a
	// token that only looks limited.
	using unique_token_windows = std::unique_ptr< void, handle_closer >;

	// Kill-on-close is the no-orphan guarantee: closing the last handle to the
	// job terminates every process inside it.
	using unique_job_windows = std::unique_ptr< void, handle_closer >;

	// The deny-everywhere descriptor the write roots are measured against.
	using unique_descriptor_windows = std::unique_ptr< void, descriptor_closer >;

	// The attribute-list numbers for the token and the Job. PROC_THREAD_
	// ATTRIBUTE_TOKEN is not declared in any SDK header, so the value is
	// spelled here; the Job value is 13 and the PROC_THREAD_ATTRIBUTE_INPUT
	// flag (0x00020000) marks both as input attributes.
	inline constexpr std::uint16_t PROC_THREAD_ATTRIBUTE_TOKEN_NUMBER = 5;
	inline constexpr std::uint16_t PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER = 13;
	inline constexpr std::uint32_t PROC_THREAD_ATTRIBUTE_INPUT_FLAG = 0x00020000;

	// The launcher initializer that hands the child its restricted token
	// and its Job Object through the process attribute list. Both handles
	// are owned by the caller's state struct, which outlives the spawn.
	struct sandbox_windows_initializer {
		HANDLE token = nullptr;
		HANDLE job = nullptr;

		auto on_setup( boost::process::v2::windows::default_launcher& launcher,
			const std::filesystem::path&, const std::wstring& ) -> boost::system::error_code {
			// The attribute list is built here rather than by the caller so
			// the two attribute numbers stay next to the handles they wrap.
			auto size = SIZE_T{ 0 };

			(void)::InitializeProcThreadAttributeList( nullptr, 2, 0, &size );

			storage.resize( size );
			attribute_list = reinterpret_cast< LPPROC_THREAD_ATTRIBUTE_LIST >(
				storage.data( ) );

			if ( !::InitializeProcThreadAttributeList( attribute_list, 2, 0, &size ) ) {
				return boost::system::error_code{ static_cast< int >( ::GetLastError( ) ),
					boost::system::system_category( ) };
			}

			if ( !::UpdateProcThreadAttribute( attribute_list, 0,
				static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_TOKEN_NUMBER ) |
					static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_INPUT_FLAG ),
				&token, sizeof( token ), nullptr, nullptr ) ) {
				return boost::system::error_code{ static_cast< int >( ::GetLastError( ) ),
					boost::system::system_category( ) };
			}

			if ( !::UpdateProcThreadAttribute( attribute_list, 0,
				static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER ) |
					static_cast< DWORD_PTR >( PROC_THREAD_ATTRIBUTE_INPUT_FLAG ),
				&job, sizeof( job ), nullptr, nullptr ) ) {
				return boost::system::error_code{ static_cast< int >( ::GetLastError( ) ),
					boost::system::system_category( ) };
			}

			launcher.startup_info.lpAttributeList = attribute_list;

			return { };
		}

		auto on_error( boost::process::v2::windows::default_launcher& launcher,
			const std::filesystem::path&, const std::wstring& ) -> void {
			if ( launcher.startup_info.lpAttributeList == attribute_list ) {
				launcher.startup_info.lpAttributeList = nullptr;
				::DeleteProcThreadAttributeList( attribute_list );
				attribute_list = nullptr;
			}
		}

		std::vector< unsigned char > storage;
		LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
	};

	// The sandbox identity, in SDDL form, for WFP scoping and diagnostics.
	[[nodiscard]] auto sandbox_windows_sid_string( ) -> std::wstring;

#endif

	// A restricted copy of the caller's own token: DISABLE_MAX_PRIVILEGE, every
	// non-logon group deny-only, and the logon SID as the sole restricting SID.
	// CreateProcessAsUserW accepts it without SeAssignPrimaryTokenPrivilege
	// precisely because it is a restricted version of the caller's token.
	[[nodiscard]] auto sandbox_windows_restricted_token( )
		-> result< unique_token_windows >;

	// A Job Object with kill-on-close, an active-process cap, a memory cap and
	// UI restrictions. The child is assigned at spawn; nothing escapes the
	// parent's death.
	[[nodiscard]] auto sandbox_windows_job_create( ) -> result< unique_job_windows >;

	// A self-relative descriptor whose DACL denies the sandbox SID everything,
	// inheritable down the tree. The write grant on each write root is what
	// pokes holes in it; a grant on a parent directory would grant the whole
	// subtree, so grants are applied per leaf the profile names.
	[[nodiscard]] auto sandbox_windows_denied_everywhere( const void* sandbox_sid )
		-> result< unique_descriptor_windows >;

	// Grants the sandbox SID write (and read/execute) on `root` and its
	// subtree, by rebuilding the root's DACL with an explicit allow ACE ahead
	// of the copied ACEs. The deny descriptor stays authoritative everywhere
	// else on the volume.
	[[nodiscard]] auto sandbox_windows_grant_write( const std::filesystem::path& root,
		const void* sandbox_sid ) -> status;

}
