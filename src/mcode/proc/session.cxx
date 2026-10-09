#include "mcode/proc/session.hxx"

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#if defined( _WIN32 )
#include "mcode/platform/sandbox_windows.hxx"
#else
#include "mcode/platform/sandbox_launcher.hxx"
#endif

namespace mcode::proc {

	namespace {

		namespace asio = boost::asio;
		namespace process = boost::process::v2;

		struct chunk {
			std::string data;
			std::string detail;
		};

		auto append_bounded( std::string& ring, const char* data, const std::size_t length )
			-> void {
			const auto room = SESSION_STDERR_RING_BYTES - ring.size( );

			if ( length > room ) {
				const auto overflow = length - room;
				ring.erase( 0, std::min( overflow, ring.size( ) ) );
			}

			ring.append( data, std::min( length, SESSION_STDERR_RING_BYTES ) );
		}

		// same contract as process.cxx: the launcher inherits the parent's directory unless
		// one is passed, and an empty path would make the child's chdir fail
		[[nodiscard]] auto session_directory( const session_options& options )
			-> std::filesystem::path {
			if ( !options.working_directory.empty( ) ) {
				return std::filesystem::path{ options.working_directory };
			}

			auto error = std::error_code{ };
			auto current = std::filesystem::current_path( error );

			return error ? std::filesystem::path{ } : current;
		}

	}

	struct session::state {
		asio::io_context context;
		process::process child{ context };
		asio::steady_timer timer{ context };

		asio::writable_pipe stdin_pipe{ context };
		asio::readable_pipe stdout_pipe{ context };

#if defined( _WIN32 )
		// kill-on-close on the Job is what guarantees the child does not outlive the session
		platform::unique_job_windows token;
		platform::unique_job_windows job;
#endif

		std::thread drainer;
		std::mutex ring_mutex;
		std::condition_variable ring_ready;
		std::deque< std::string > pending;
		std::string ring;
		bool draining_done = false;

		~state( ) {
			if ( drainer.joinable( ) ) {
				drainer.join( );
			}
		}
	};

	session::session( session&& other ) noexcept = default;

	session::~session( ) {
		if ( !state_ ) {
			return;
		}

		// children must not outlive mcode; the destructor is what guarantees it
		auto ignored = boost::system::error_code{ };

		// a close failure means the pipe was already gone; the exit path is the real guarantee
		state_->stdin_pipe.close( ignored );

		if ( state_->child.running( ignored ) ) {
			const auto exited = wait_exit( SESSION_GRACE_WAIT );

			if ( !exited ) {
				state_->child.terminate( ignored );
				wait_exit( SESSION_KILL_WAIT );
			}
		}

		finalize( );
	}

	auto session::operator=( session&& other ) noexcept -> session& = default;

	auto session::spawn( const session_options& options ) -> result< session > {
		if ( options.executable.empty( ) ) {
			return std::unexpected( fail( errc::io, "no executable given" ) );
		}

		auto owned = session{ };
		owned.state_ = std::make_unique< state >( );

		auto& context = owned.state_->context;
		auto& child = owned.state_->child;

		// named lvalues, not temporaries: a moved-from one leaves a stale inherit-list handle
		auto stderr_pipe = asio::readable_pipe{ context };

		try {
			// not the popen ctor: its second on_setup overwrites handles; use one process_stdio
#if defined( _WIN32 )
			if ( options.sandbox != nullptr ) {
				auto token = platform::sandbox_windows_child_token( );

				if ( !token ) {
					return std::unexpected( token.error( ) );
				}

				auto job = platform::sandbox_windows_job_create( );

				if ( !job ) {
					return std::unexpected( job.error( ) );
				}

				if ( const auto marked = platform::sandbox_windows_mark_write_paths(
					options.sandbox->write_paths, options.sandbox->deny_paths ); !marked ) {
					return std::unexpected( marked.error( ) );
				}

				owned.state_->token = std::move( *token );
				owned.state_->job = std::move( *job );

				auto initializer = platform::sandbox_windows_initializer{
					owned.state_->job.get( ) };

				child = process::default_process_launcher( )( context, options.executable,
					options.args,
					process::process_stdio{ owned.state_->stdin_pipe, owned.state_->stdout_pipe,
						stderr_pipe },
					boost::process::v2::process_start_dir{ session_directory( options ) },
					initializer );
			} else {
				child = process::default_process_launcher( )( context, options.executable,
					options.args,
					process::process_stdio{ owned.state_->stdin_pipe, owned.state_->stdout_pipe,
						stderr_pipe },
					boost::process::v2::process_start_dir{ session_directory( options ) } );
			}
#elif defined( __linux__ ) || defined( __APPLE__ )
			child = process::default_process_launcher( )( context, options.executable,
				options.args,
				process::process_stdio{ owned.state_->stdin_pipe, owned.state_->stdout_pipe,
					stderr_pipe },
				boost::process::v2::process_start_dir{ session_directory( options ) },
				platform::sandbox_posix_initializer{ options.sandbox } );
#else
			child = process::default_process_launcher( )( context, options.executable,
				options.args,
				process::process_stdio{ owned.state_->stdin_pipe, owned.state_->stdout_pipe,
					stderr_pipe },
				boost::process::v2::process_start_dir{ session_directory( options ) } );
#endif
		} catch ( const boost::system::system_error& exception ) {
			return std::unexpected(
				fail( errc::io, std::string{ "spawn failed: " } + exception.what( ) ) );
		}

		auto* state_pointer = owned.state_.get( );

		state_pointer->drainer = std::thread(
			[ state_pointer, pipe = std::move( stderr_pipe ) ]( ) mutable {
			auto buffer = std::array< char, SESSION_CHUNK_BYTES >{ };

			while ( true ) {
				auto moved_error = boost::system::error_code{ };
				const auto length = pipe.read_some( asio::buffer( buffer ), moved_error );

				if ( length > 0 ) {
					auto text = std::string{ buffer.data( ), length };

					{
						auto guard = std::lock_guard< std::mutex >( state_pointer->ring_mutex );
						state_pointer->pending.push_back( std::move( text ) );
					}

					state_pointer->ring_ready.notify_one( );
				}

				if ( moved_error ) {
					break;
				}
			}

			{
				auto guard = std::lock_guard< std::mutex >( state_pointer->ring_mutex );
				state_pointer->draining_done = true;
			}

			state_pointer->ring_ready.notify_all( );
		} );

		return owned;
	}

	auto session::write( const std::string_view bytes ) -> status {
		if ( !state_ ) {
			return std::unexpected( fail( errc::io, "no child process" ) );
		}

		auto written = std::size_t{ 0 };

		while ( written < bytes.size( ) ) {
			auto error = boost::system::error_code{ };
			const auto chunk = state_->stdin_pipe.write_some(
				asio::buffer( bytes.data( ) + written, bytes.size( ) - written ), error );

			written += chunk;

			if ( error ) {
				return std::unexpected(
					fail( errc::io, "stdin write failed: " + error.message( ) ) );
			}

			if ( chunk == 0 ) {
				return std::unexpected( fail( errc::io, "stdin write made no progress" ) );
			}
		}

		return { };
	}

	auto session::close_stdin( ) -> status {
		if ( !state_ ) {
			return std::unexpected( fail( errc::io, "no child process" ) );
		}

		auto error = boost::system::error_code{ };
		state_->stdin_pipe.close( error );

		if ( error ) {
			return std::unexpected(
				fail( errc::io, "stdin close failed: " + error.message( ) ) );
		}

		return { };
	}

	auto session::read_some( const std::chrono::milliseconds timeout ) -> result< read_result > {
		if ( !state_ ) {
			return std::unexpected( fail( errc::io, "no child process" ) );
		}

		auto outcome = read_result{ };
		auto settled = false;

		auto buffer = std::array< char, SESSION_CHUNK_BYTES >{ };

		state_->timer.expires_after( timeout );

		state_->stdout_pipe.async_read_some( asio::buffer( buffer ),
			[ & ]( const boost::system::error_code& error, const std::size_t length ) {
				if ( error ) {
					if ( error == asio::error::operation_aborted ) {
						outcome.kind = read_kind::timeout;
					} else {
						outcome.kind = read_kind::eof;
						outcome.detail = error.message( );
					}

					settled = true;

					// Cancelled here too. Without it the timer stays pending and
					// `context.run` below blocks until it fires, so a pipe that hit
					// EOF immediately was still reported a full timeout later.
					state_->timer.cancel( );

					return;
				}

				outcome.kind = read_kind::data;
				outcome.data.assign( buffer.data( ), length );
				settled = true;

				state_->timer.cancel( );
			} );

		state_->timer.async_wait( [ & ]( const boost::system::error_code& error ) {
			if ( !error && !settled ) {
				state_->stdout_pipe.cancel( );
			}
		} );

		state_->context.restart( );
		state_->context.run( );

		return outcome;
	}

	auto session::wait_exit( const std::chrono::milliseconds timeout ) -> std::optional< int > {
		if ( !state_ ) {
			return std::nullopt;
		}

		const auto deadline = std::chrono::steady_clock::now( ) + timeout;

		while ( std::chrono::steady_clock::now( ) < deadline ) {
			auto error = boost::system::error_code{ };
			const auto alive = state_->child.running( error );

			if ( error ) {
				return std::nullopt;
			}

			if ( !alive ) {
				return static_cast< int >( state_->child.native_exit_code( ) );
			}

			std::this_thread::sleep_for( SESSION_EXIT_POLL );
		}

		return std::nullopt;
	}

	auto session::terminate( ) -> void {
		if ( !state_ ) {
			return;
		}

		auto ignored = boost::system::error_code{ };
		state_->child.terminate( ignored );
		wait_exit( SESSION_KILL_WAIT );
	}

	auto session::running( ) -> bool {
		if ( !state_ ) {
			return false;
		}

		auto error = boost::system::error_code{ };

		return state_->child.running( error );
	}

	auto session::id( ) const noexcept -> std::uint64_t {
		if ( !state_ ) {
			return 0;
		}

		return static_cast< std::uint64_t >( state_->child.id( ) );
	}

	auto session::stderr_text( ) const -> std::string {
		if ( !state_ ) {
			return { };
		}

		auto guard = std::lock_guard< std::mutex >( state_->ring_mutex );

		// drain what the drainer staged, or a read between chunks sees an empty ring
		while ( !state_->pending.empty( ) ) {
			append_bounded( state_->ring, state_->pending.front( ).data( ),
				state_->pending.front( ).size( ) );
			state_->pending.pop_front( );
		}

		return state_->ring;
	}

	auto session::finalize( ) -> void {
		if ( !state_ ) {
			return;
		}

		if ( state_->drainer.joinable( ) ) {
			state_->drainer.join( );
		}

		auto guard = std::lock_guard< std::mutex >( state_->ring_mutex );

		while ( !state_->pending.empty( ) ) {
			append_bounded( state_->ring, state_->pending.front( ).data( ),
				state_->pending.front( ).size( ) );
			state_->pending.pop_front( );
		}
	}

}
