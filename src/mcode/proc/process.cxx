#include "mcode/proc/process.hxx"

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/exit_code.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>

#if defined( _WIN32 )
#include <processthreadsapi.h>
#include "mcode/platform/sandbox_windows.hxx"
#endif

namespace mcode {

	namespace {

		namespace asio = boost::asio;
		namespace process = boost::process::v2;

		struct pipe_drain {
			asio::readable_pipe pipe;
			std::string data;
			bool truncated = false;
			std::size_t capacity = 0;

			pipe_drain( asio::io_context& context, const std::size_t limit )
				: pipe( context ), capacity( limit ) { }
		};

		[[nodiscard]] auto read_environment( const char* name ) -> std::string {
		#if defined( _WIN32 )
			char* value = nullptr;
			auto size = std::size_t{ 0 };

			if ( _dupenv_s( &value, &size, name ) != 0 || value == nullptr ) {
				return { };
			}

			auto out = std::string{ value };
			std::free( value );

			return out;
		#else
			const auto* value = std::getenv( name );

			return value != nullptr ? std::string{ value } : std::string{ };
		#endif
		}

		auto append_bounded( pipe_drain& drain, const char* data, const std::size_t length ) -> void {
			if ( drain.data.size( ) >= drain.capacity ) {
				drain.truncated = true;

				return;
			}

			const auto room = drain.capacity - drain.data.size( );
			drain.data.append( data, std::min( length, room ) );
		}

#if defined( _WIN32 )
		using mcode::platform::PROC_THREAD_ATTRIBUTE_TOKEN_NUMBER;
		using mcode::platform::PROC_THREAD_ATTRIBUTE_JOB_LIST_NUMBER;
		using mcode::platform::PROC_THREAD_ATTRIBUTE_INPUT_FLAG;
		// The launcher initializer that hands the child its restricted token
		// and its Job Object through the process attribute list. Both handles
		// are owned by the caller's state struct, which outlives the spawn.
		struct sandbox_windows_initializer {
			HANDLE token = nullptr;
			HANDLE job = nullptr;

			auto on_setup( process::windows::default_launcher& launcher,
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

			auto on_error( process::windows::default_launcher& launcher,
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
#endif

	}

	auto minimal_environment( ) -> std::map< std::string, std::string, std::less<> > {
		auto environment = std::map< std::string, std::string, std::less<> >{ };

	#if defined( _WIN32 )
		for ( const auto* name : { "SYSTEMROOT", "PATH", "TEMP", "PATHEXT", "COMSPEC" } ) {
			if ( auto value = read_environment( name ); !value.empty( ) ) {
				environment[ name ] = std::move( value );
			}
		}
	#else
		for ( const auto* name : { "PATH", "HOME" } ) {
			if ( auto value = read_environment( name ); !value.empty( ) ) {
				environment[ name ] = std::move( value );
			}
		}

		environment[ "LANG" ] = "C.UTF-8";
	#endif

		return environment;
	}

	auto find_executable( const std::string_view name ) -> result< std::string > {
		try {
			const auto path = process::environment::find_executable( std::string{ name } );

			if ( path.empty( ) ) {
				return std::unexpected(
					fail( errc::io, "executable not found on PATH: " + std::string{ name } ) );
			}

			return path.string( );
		} catch ( const boost::system::system_error& exception ) {
			return std::unexpected(
				fail( errc::io, std::string{ "PATH lookup failed: " } + exception.what( ) ) );
		}
	}

	auto run_process( const process_options& options ) -> result< process_result > {
		if ( options.executable.empty( ) ) {
			return std::unexpected( fail( errc::io, "no executable given" ) );
		}

		const auto started = std::chrono::steady_clock::now( );

		try {
			auto context = asio::io_context{ };

			auto out = pipe_drain{ context, options.max_output_bytes };
			auto err = pipe_drain{ context, options.max_output_bytes };
			auto in = asio::writable_pipe{ context };

			auto environment_strings = std::vector< std::string >{ };

			{
				auto environment = options.scrub_environment
					? minimal_environment( )
					: std::map< std::string, std::string, std::less<> >{ };

				for ( const auto& [ key, value ] : options.environment ) {
					environment[ key ] = value;
				}

				environment_strings.reserve( environment.size( ) );

				for ( const auto& [ key, value ] : environment ) {
					environment_strings.push_back( key + "=" + value );
				}
			}

			auto environment = process::process_environment{ std::move( environment_strings ) };

			auto child = process::process{ context, options.executable, options.args,
				process::process_stdio{ in, out.pipe, err.pipe }, environment };

			in.close( );

			auto out_buffer = std::array< char, PIPE_CHUNK_BYTES >{ };
			auto err_buffer = std::array< char, PIPE_CHUNK_BYTES >{ };
			auto read_out = std::function< void( ) >{ };
			auto read_err = std::function< void( ) >{ };

			auto active_reads = std::make_shared< int >( 2 );
			auto timer = asio::steady_timer{ context };
			auto timed_out = false;

			auto on_read_done = [&]( ) {
				if ( --( *active_reads ) == 0 ) {
					timer.cancel( );
				}
			};

			read_out = [&]( ) {
				out.pipe.async_read_some( asio::buffer( out_buffer ),
					[&]( const boost::system::error_code& error_code, const std::size_t length ) {
						if ( error_code ) {
							on_read_done( );

							return;
						}

						append_bounded( out, out_buffer.data( ), length );
						read_out( );
					} );
			};

			read_err = [&]( ) {
				err.pipe.async_read_some( asio::buffer( err_buffer ),
					[&]( const boost::system::error_code& error_code, const std::size_t length ) {
						if ( error_code ) {
							on_read_done( );

							return;
						}

						append_bounded( err, err_buffer.data( ), length );
						read_err( );
					} );
			};

			read_out( );
			read_err( );

			timer.expires_after( options.timeout );
			timer.async_wait( [&]( const boost::system::error_code& error_code ) {
				if ( error_code ) {
					return;
				}

				timed_out = true;

				auto ignored = boost::system::error_code{ };
				out.pipe.cancel( ignored );
				err.pipe.cancel( ignored );
				out.pipe.close( ignored );
				err.pipe.close( ignored );
			} );

			context.run( );

			if ( timed_out ) {
				auto ignored = boost::system::error_code{ };
				child.terminate( ignored );
			}

			auto wait_error = boost::system::error_code{ };
			const auto exit_status = child.wait( wait_error );

			if ( wait_error ) {
				return std::unexpected( fail( errc::io, "wait failed: " + wait_error.message( ) ) );
			}

			auto result = process_result{ };
			result.exit_code = exit_status;
			result.stdout_text = std::move( out.data );
			result.stderr_text = std::move( err.data );
			result.timed_out = timed_out;
			result.output_truncated = out.truncated || err.truncated;
			result.elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ) - started );

			return result;
		} catch ( const boost::system::system_error& exception ) {
			return std::unexpected(
				fail( errc::io, std::string{ "spawn failed: " } + exception.what( ) ) );
		}
	}

}
