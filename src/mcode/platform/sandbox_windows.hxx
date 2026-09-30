#pragma once

// Windows half of the Sandbox seam: Low integrity level, privilege strip,
// and the Job Object. Writes are confined by mandatory integrity policy --
// a Low IL child cannot write to a Medium IL object -- and the write_paths
// subtrees are marked Low IL so the child can write exactly there. Reads are
// NOT confined on this platform; the capability enum says write_boundary.

#include <cstdint>
#include <filesystem>
#include <map>
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

	// Kill-on-close is the no-orphan guarantee: closing the last handle to the
	// job terminates every process inside it.
	using unique_job_windows = std::unique_ptr< void, handle_closer >;

	// The attribute-list number for the Job. The Low IL token is NOT applied
	// through the attribute list yet: nothing in the spawn path calls
	// CreateProcessAsUserW, so the child currently receives the caller's full
	// token and only the Job Object is enforced. The Job value is 13 and the
	// PROC_THREAD_ATTRIBUTE_INPUT flag (0x00020000) marks it as an input
	// attribute.
	inline constexpr std::uint16_t PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER = 13;
	inline constexpr std::uint32_t PROC_THREAD_ATTRIBUTE_INPUT_FLAG = 0x00020000;

	// The launcher initializer that assigns the child to the Job Object
	// through the process attribute list. The Job handle is owned by the
	// caller, which must outlive the spawn call.
	struct sandbox_windows_initializer {
		HANDLE job = nullptr;

		auto on_setup( boost::process::v2::windows::default_launcher& launcher,
			const std::filesystem::path&, const std::wstring& ) -> boost::system::error_code {
			// An unsandboxed spawn carries no Job. Adding a null handle to the
			// attribute list fails the spawn, so the no-op case returns before
			// touching the launcher and leaves its default startup info alone.
			if ( job == nullptr ) {
				return { };
			}

			auto size = SIZE_T{ 0 };

			(void)::InitializeProcThreadAttributeList( nullptr, 1, 0, &size );

			storage.resize( size );
			attribute_list = reinterpret_cast< LPPROC_THREAD_ATTRIBUTE_LIST >(
				storage.data( ) );

			if ( !::InitializeProcThreadAttributeList( attribute_list, 1, 0, &size ) ) {
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
			// Nothing was built when the initializer was a no-op, and a null
			// list compares equal to the launcher's default.
			if ( attribute_list == nullptr ) {
				return;
			}

			if ( launcher.startup_info.lpAttributeList == attribute_list ) {
				launcher.startup_info.lpAttributeList = nullptr;
				::DeleteProcThreadAttributeList( attribute_list );
				attribute_list = nullptr;
			}
		}

		std::vector< unsigned char > storage = { };
		LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
	};

#endif

	// The child token: a duplicate of the caller's primary token with every
	// privilege stripped and the integrity level set to Low. Writes to Medium
	// IL objects fail; reads still work, which is why the capability is
	// write_boundary and not filesystem.
	[[nodiscard]] auto sandbox_windows_child_token( )
		-> result< unique_job_windows >;

	// A Job Object with kill-on-close, an active-process cap, a memory cap and
	// UI restrictions.
	[[nodiscard]] auto sandbox_windows_job_create( ) -> result< unique_job_windows >;

	// Marks each write path Low IL so the child can write there, and each
	// deny path Medium IL so it cannot. The mandatory label is the whole
	// write boundary: a Low IL process denied write-up everywhere it was not
	// explicitly lowered.
	[[nodiscard]] auto sandbox_windows_mark_write_paths(
		const std::vector< std::filesystem::path >& write_paths,
		const std::vector< std::filesystem::path >& deny_paths ) -> status;

	// One set of stdio pipes for the raw spawn: the child ends are
	// inheritable, the parent ends are owned by the caller and assigned to
	// its asio pipes. Null after handover.
	struct sandbox_raw_pipes {
		void* child_stdin = nullptr;
		void* child_stdout = nullptr;
		void* child_stderr = nullptr;
		void* parent_stdin = nullptr;
		void* parent_stdout = nullptr;
		void* parent_stderr = nullptr;
	};

	[[nodiscard]] auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes >;

	// One spawned sandboxed child: the pid and the raw process handle. The
	// caller wraps them in its own process object; the Job handle stays with
	// the caller and kill-on-close does the rest.
	struct sandbox_spawn_windows {
		std::uint32_t process_id = 0;
		void* process_handle = nullptr;
	};

	// Spawns the executable with the Low IL token through
	// CreateProcessAsUserW, with the given stdio handles inherited. The
	// attribute list assigns the Job at creation. argv is quoted the way the
	// CRT expects; no shell is involved.
	[[nodiscard]] auto sandbox_windows_spawn( const std::filesystem::path& executable,
		const std::vector< std::string >& arguments,
		const std::filesystem::path& working_directory,
		const std::map< std::string, std::string, std::less<> >& environment,
		const void* stdin_read, const void* stdout_write, const void* stderr_write,
		void* job, const void* token ) -> result< sandbox_spawn_windows >;

}
