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
#include <sddl.h>
#include "mcode/platform/sandbox_windows.hxx"
#else
#include "mcode/platform/sandbox_launcher.hxx"
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
		// Everything one sandboxed child needs, held until the spawn returns
		// and the Job handle takes over ownership of the child's lifetime.
		struct sandbox_spawn_state {
			platform::unique_job_windows token;
			platform::unique_job_windows job;

			// The parent's write end of the child's stdin, held raw until the
			// child exits and closed explicitly after the wait.
			void* parent_stdin = nullptr;
		};

		[[nodiscard]] auto make_sandbox_spawn_state( const platform::sandbox_profile& profile )
			-> result< sandbox_spawn_state > {
			auto token = platform::sandbox_windows_child_token( );

			if ( !token ) {
				return std::unexpected( token.error( ) );
			}

			auto job = platform::sandbox_windows_job_create( );

			if ( !job ) {
				return std::unexpected( job.error( ) );
			}

			// The write boundary is the mandatory integrity label: write paths
			// are lowered to Low IL so the child can write them, deny paths
			// stay Medium so it cannot.
			if ( const auto marked = platform::sandbox_windows_mark_write_paths(
				profile.write_paths, profile.deny_paths ); !marked ) {
				return std::unexpected( marked.error( ) );
			}

			return sandbox_spawn_state{ std::move( *token ), std::move( *job ) };
		}
#endif

#if defined( __linux__ ) || defined( __APPLE__ )
		// The POSIX form: the profile pointer drives the fork-exec hook, and
		// null spawns unsandboxed. `sandbox_spawn_state` is Windows-only -- it
		// holds a token and a Job handle -- so this overload ignores it and
		// takes the state by value rather than naming a type it cannot see.
		[[nodiscard]] auto sandbox_initializer( const std::optional< int >&,
			const platform::sandbox_profile* profile )
			-> platform::sandbox_posix_initializer {
			return platform::sandbox_posix_initializer{ profile };
		}
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

			auto environment_map = std::map< std::string, std::string, std::less<> >{ };

			{
				auto environment = options.scrub_environment
					? minimal_environment( )
					: std::map< std::string, std::string, std::less<> >{ };

				for ( const auto& [ key, value ] : options.environment ) {
					environment[ key ] = value;
				}

				environment_map = environment;
			}

			auto environment_strings = std::vector< std::string >{ };
			environment_strings.reserve( environment_map.size( ) );

			for ( const auto& [ key, value ] : environment_map ) {
				environment_strings.push_back( key + "=" + value );
			}

			auto environment = process::process_environment{ std::move( environment_strings ) };

#if defined( _WIN32 )
			// The sandbox state must outlive the spawn call: the Job handle is
			// what kills the child when this process dies, and the attribute
			// list must stay valid until CreateProcessW has consumed it.
			auto sandbox_state = std::optional< sandbox_spawn_state >{ };

			if ( options.sandbox != nullptr ) {
				auto state = make_sandbox_spawn_state( *options.sandbox );

				if ( !state ) {
					return std::unexpected( state.error( ) );
				}

				sandbox_state = std::move( *state );
			}
#else
			// POSIX applies the profile in the child after fork, so the parent
			// keeps nothing: the initializer carries the pointer and the
			// platform layer does the rest.
			auto sandbox_state = std::optional< int >{ };
#endif

#if defined( _WIN32 )
			std::optional< process::process > child{ };

			// The raw handles the sandboxed spawn inherits. The parent ends
			// are assigned to the asio pipes afterwards; the child ends are
			// inheritable and owned by the child once spawned.
			void* in_handle = nullptr;
			void* out_handle = nullptr;
			void* err_handle = nullptr;

			auto raw_pipes = std::optional< platform::sandbox_raw_pipes >{ };

			if ( sandbox_state ) {
				auto pipes = platform::sandbox_windows_make_pipes( );

				if ( !pipes ) {
					return std::unexpected( pipes.error( ) );
				}

				raw_pipes = std::move( *pipes );

				in_handle = raw_pipes->child_stdin;
				out_handle = raw_pipes->child_stdout;
				err_handle = raw_pipes->child_stderr;
			}

			if ( sandbox_state ) {
				// The sandboxed path spawns through CreateProcessAsUserW with
				// the Low IL token; boost's launcher has no token parameter,
				// so the stdio pipes are created here and handed over raw.
				auto raw = platform::sandbox_windows_spawn(
					std::filesystem::path{ options.executable }, options.args,
					std::filesystem::path{ options.working_directory },
					environment_map, in_handle, out_handle, err_handle,
					sandbox_state->job.get( ), sandbox_state->token.get( ) );

				if ( !raw ) {
					return std::unexpected( raw.error( ) );
				}

				child.emplace( context, raw->process_id,
					static_cast< process::process::native_handle_type >( raw->process_handle ) );

				out.pipe.assign( raw_pipes->parent_stdout );
				err.pipe.assign( raw_pipes->parent_stderr );

				// The child's stdin is NOT assigned to an asio pipe: the IOCP
				// association alters the handle in ways that break the
				// child's console initialization, and a one-shot command
				// never writes to stdin anyway. The parent holds the write
				// end open until the child exits, which is what keeps the
				// pipe alive.
				sandbox_state->parent_stdin = raw_pipes->parent_stdin;

				raw_pipes->parent_stdin = nullptr;
				raw_pipes->parent_stdout = nullptr;
				raw_pipes->parent_stderr = nullptr;
			} else {
				child.emplace( context, options.executable, options.args,
					process::process_stdio{ in, out.pipe, err.pipe }, environment );
			}
#else
			// An optional, like the Windows branch: the drain loop below is
			// shared and reaches the child through `child->`.
			auto child = std::optional< process::process >{ };
			child.emplace( context, options.executable, options.args,
				process::process_stdio{ in, out.pipe, err.pipe }, environment,
				sandbox_initializer( sandbox_state, options.sandbox ) );
#endif

#if defined( _WIN32 )
			// The sandboxed child's stdin is a named-pipe server end. Closing
			// the parent's client end now would disconnect the pipe and the
			// child's reads would fail, so it stays open until the child has
			// exited; the boost path relies on close-for-EOF instead.
			if ( !sandbox_state ) {
				in.close( );
			}
#else
			in.close( );
#endif

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
				child->terminate( ignored );
			}

			// The child is gone; the stdin write end can go with it.
#if defined( _WIN32 )
			if ( sandbox_state && sandbox_state->parent_stdin != nullptr ) {
				::CloseHandle( static_cast< HANDLE >( sandbox_state->parent_stdin ) );
				sandbox_state->parent_stdin = nullptr;
			}
#endif

			auto wait_error = boost::system::error_code{ };
			const auto exit_status = child->wait( wait_error );

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
