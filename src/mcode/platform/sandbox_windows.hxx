#pragma once

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

	// kill-on-close: closing the last handle terminates every process in the job.
	using unique_job_windows = std::unique_ptr< void, handle_closer >;

#endif

	// parses on other platforms; never constructed there.
#if !defined( _WIN32 )
	struct unique_job_windows_placeholder { };
	using unique_job_windows = unique_job_windows_placeholder;
#endif

#if defined( _WIN32 )

	inline constexpr std::uint16_t PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER = 13;
	inline constexpr std::uint32_t PROC_THREAD_ATTRIBUTE_INPUT_FLAG = 0x00020000;

	// the Job handle is owned by the caller, which must outlive the spawn call.
	struct sandbox_windows_initializer {
		HANDLE job = nullptr;

		auto on_setup( boost::process::v2::windows::default_launcher& launcher,
			const std::filesystem::path&, const std::wstring& ) -> boost::system::error_code {
			// adding a null handle to the attribute list fails the spawn.
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

	// all privileges stripped, integrity Low: writes to Medium IL objects fail, reads work.
	[[nodiscard]] auto sandbox_windows_child_token( )
		-> result< unique_job_windows >;

	[[nodiscard]] auto sandbox_windows_job_create( ) -> result< unique_job_windows >;

	// write paths Low IL, deny paths Medium IL; the label is the write boundary.
	[[nodiscard]] auto sandbox_windows_mark_write_paths(
		const std::vector< std::filesystem::path >& write_paths,
		const std::vector< std::filesystem::path >& deny_paths ) -> status;

	// null after handover.
	struct sandbox_raw_pipes {
		void* child_stdin = nullptr;
		void* child_stdout = nullptr;
		void* child_stderr = nullptr;
		void* parent_stdin = nullptr;
		void* parent_stdout = nullptr;
		void* parent_stderr = nullptr;
	};

	[[nodiscard]] auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes >;

	struct sandbox_spawn_windows {
		std::uint32_t process_id = 0;
		void* process_handle = nullptr;
	};

	// argv is quoted the way the CRT expects; no shell is involved.
	[[nodiscard]] auto sandbox_windows_spawn( const std::filesystem::path& executable,
		const std::vector< std::string >& arguments,
		const std::filesystem::path& working_directory,
		const std::map< std::string, std::string, std::less<> >& environment,
		const void* stdin_read, const void* stdout_write, const void* stderr_write,
		void* job, const void* token ) -> result< sandbox_spawn_windows >;

}
