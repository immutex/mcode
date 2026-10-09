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

	// Any owned Win32 handle, not only a Job. Named for the general case because a
	// hand-held raw handle has no owner and leaks on the first throw past it.
	using unique_handle_windows = std::unique_ptr< void, handle_closer >;

	// kill-on-close: closing the last handle terminates every process in the job.
	using unique_job_windows = unique_handle_windows;

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
				const auto error = ::GetLastError( );
				::DeleteProcThreadAttributeList( attribute_list );
				attribute_list = nullptr;

				return boost::system::error_code{ static_cast< int >( error ),
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

	// the parent must close child_* after the spawn or its read of stdout never sees EOF.
	struct sandbox_raw_pipes {
		void* child_stdin = nullptr;
		void* child_stdout = nullptr;
		void* child_stderr = nullptr;
		void* parent_stdin = nullptr;
		void* parent_stdout = nullptr;
		void* parent_stderr = nullptr;
	};

	// closes every handle still set and nulls it, so a second call cannot double-close.
	auto sandbox_windows_close_pipes( sandbox_raw_pipes& pipes ) -> void;

	[[nodiscard]] auto sandbox_windows_make_pipes( ) -> result< sandbox_raw_pipes >;

	struct sandbox_spawn_windows {
		std::uint32_t process_id = 0;
		void* process_handle = nullptr;
	};

	// The three stream handles and the two owned handles the launcher needs. A
	// struct rather than five more positional parameters: `stdin_read`,
	// `stdout_write` and `stderr_write` were three consecutive `const void*`, so
	// passing them in the wrong order compiled silently and produced a child whose
	// stdin was its own stdout.
	struct sandbox_spawn_windows_handles {
		const void* stdin_read = nullptr;
		const void* stdout_write = nullptr;
		const void* stderr_write = nullptr;

		// not owned; both must outlive the spawn call
		void* job = nullptr;
		const void* token = nullptr;
	};

	// argv is quoted the way the CRT expects; no shell is involved.
	[[nodiscard]] auto sandbox_windows_spawn( const std::filesystem::path& executable,
		const std::vector< std::string >& arguments,
		const std::filesystem::path& working_directory,
		const std::map< std::string, std::string, std::less<> >& environment,
		const sandbox_spawn_windows_handles& handles ) -> result< sandbox_spawn_windows >;

}
