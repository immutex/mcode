#include "mcode/agent/loop.hxx"

#include <exception>
#include <fstream>

#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mcode {

	auto event::to_json( ) const -> std::string {
		auto out = std::string{ };
		out.reserve( 128 + payload_json.size( ) );

		out += "{\"v\":";
		out += std::to_string( version );
		out += ",\"seq\":";
		out += std::to_string( sequence );
		out += ",\"ts\":";
		out += std::to_string( timestamp_ms );
		out += ",\"kind\":\"";
		json::append_escaped( out, kind );
		out += "\",\"run\":\"";
		json::append_escaped( out, run );
		out += "\",\"turn\":";
		out += std::to_string( turn );
		out += ",\"step\":";
		out += std::to_string( step );
		out += ",\"payload\":";
		out += payload_json.empty( ) ? "{}" : payload_json;
		out += "}";

		return out;
	}

	event_log::~event_log( ) {
		close( );
	}

	event_log::event_log( event_log&& other ) noexcept
		: events_( std::move( other.events_ ) )
		, next_sequence_( other.next_sequence_ )
		, branch_id_( std::move( other.branch_id_ ) )
		, sink_( std::move( other.sink_ ) )
		, path_( std::move( other.path_ ) )
		, write_failures_( other.write_failures_ ) {
		other.sink_ = nullptr;
	}

	auto event_log::operator=( event_log&& other ) noexcept -> event_log& {
		if ( this != &other ) {
			close( );

			events_ = std::move( other.events_ );
			next_sequence_ = other.next_sequence_;
			branch_id_ = std::move( other.branch_id_ );
			sink_ = std::move( other.sink_ );
			path_ = std::move( other.path_ );
			write_failures_ = other.write_failures_;

			other.sink_ = nullptr;
		}

		return *this;
	}

	auto event_log::open( const std::filesystem::path& path ) -> status {
		close( );

		if ( path.has_parent_path( ) ) {
			auto error = std::error_code{ };
			std::filesystem::create_directories( path.parent_path( ), error );

			if ( error ) {
				return std::unexpected( fail( errc::io,
					"cannot create " + path.parent_path( ).string( ) + ": " + error.message( ) ) );
			}
		}

		// Whether there is prior content, sampled BEFORE the open. A file that
		// exists but cannot be replayed is not an empty file: treating it as one
		// would reissue sequences already on disk, so an unreadable log is an error
		// the caller must see.
		auto error = std::error_code{ };
		const auto prior_bytes = std::filesystem::file_size( path, error );
		const auto had_content = !error && prior_bytes > 0;

		// Append mode, and never truncate: a resumed session continues the same
		// file so the whole run stays in one place.
		auto* handle = std::fopen( path.string( ).c_str( ), "ab" );

		if ( handle == nullptr ) {
			return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
		}

		sink_ = std::unique_ptr< std::FILE, void ( * )( std::FILE* ) >{ handle, []( std::FILE* file ) {
			std::fclose( file );
		} };
		path_ = path;

		if ( !had_content ) {
			return { };
		}

		// Adopt the existing file's sequence numbers, so a resumed session does not
		// restart at zero and collide with what is already on disk.
		auto existing = replay_event_log( path );

		if ( !existing ) {
			close( );

			return std::unexpected( fail( errc::io, "cannot replay the existing log " +
				path.string( ) + ": " + existing.error( ).msg ) );
		}

		// Even a log whose every line is malformed still occupies sequences.
		next_sequence_ = existing->log.next_sequence( );

		return { };
	}

	auto event_log::close( ) -> void {
		if ( sink_ != nullptr ) {
			std::fflush( sink_.get( ) );
			sink_.reset( );
		}
	}

	auto event_log::append( std::string kind, std::string payload_json ) -> event {
		auto appended = event{ };
		appended.sequence = next_sequence_++;
		appended.timestamp_ms = support::epoch_milliseconds( );
		appended.kind = std::move( kind );
		appended.payload_json = std::move( payload_json );
		appended.run = branch_id_;

		if ( sink_ != nullptr ) {
			// Write and flush per event. A buffered log loses exactly the events
			// leading up to a crash, which are the ones a post-mortem needs.
			const auto line = appended.to_json( ) + "\n";
			const auto written = std::fwrite( line.data( ), 1, line.size( ), sink_.get( ) );

			if ( written != line.size( ) || std::fflush( sink_.get( ) ) != 0 ) {
				++write_failures_;
			}
		}

		events_.push_back( std::move( appended ) );

		return events_.back( );
	}

	auto event_log::restore( event recorded ) -> void {
		// The next append must not reuse a sequence already on disk.
		if ( recorded.sequence >= next_sequence_ ) {
			next_sequence_ = recorded.sequence + 1;
		}

		events_.push_back( std::move( recorded ) );
	}

	auto event_log::to_jsonl( ) const -> std::string {
		auto out = std::string{ };

		for ( const auto& entry : events_ ) {
			out += entry.to_json( );
			out += '\n';
		}

		return out;
	}

	auto replay_event_log( const std::filesystem::path& path ) -> result< replay_result > {
		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
		}

		auto result = replay_result{ };
		auto line = std::string{ };

		while ( std::getline( stream, line ) ) {
			// A final line with no trailing newline is a torn write: the process died
			// mid-flush. Reported, not fatal -- the rest of the session is intact and
			// is exactly the evidence a post-mortem wants.
			if ( stream.eof( ) && !line.empty( ) ) {
				result.truncated_tail = true;

				break;
			}

			if ( line.empty( ) ) {
				continue;
			}

			auto parsed = json::document::parse( line );

			if ( !parsed ) {
				++result.malformed_lines;

				continue;
			}

			auto sequence = parsed->get_int( "seq" );
			auto kind = parsed->get_string( "kind" );

			if ( !sequence || !kind ) {
				++result.malformed_lines;

				continue;
			}

			// Rebuilt with its recorded envelope, so a replayed event IS
			// indistinguishable from a live one. Going through append() here would
			// renumber it and discard the payload -- which is the one thing a
			// post-mortem needs.
			auto recorded = event{ };
			recorded.sequence = static_cast< std::uint64_t >( *sequence );
			recorded.kind = *kind;

			if ( auto timestamp = parsed->get_int( "ts" ) ) {
				recorded.timestamp_ms = *timestamp;
			}

			if ( auto run = parsed->get_string( "run" ) ) {
				recorded.run = *run;
			}

			if ( auto turn = parsed->get_int( "turn" ) ) {
				recorded.turn = static_cast< std::uint32_t >( *turn );
			}

			if ( auto step = parsed->get_int( "step" ) ) {
				recorded.step = static_cast< std::uint32_t >( *step );
			}

			if ( auto payload = parsed->pointer_raw( "/payload" ) ) {
				recorded.payload_json = *payload;
			}

			result.log.restore( std::move( recorded ) );

			++result.events_read;
		}

		return result;
	}

	auto agent_loop::register_handler( std::string name, tool_handler handler ) -> void {
		// Re-registering a name replaces the handler rather than appending a second
		// entry, so a reload cannot leave two implementations competing.
		handlers_[ std::move( name ) ] = std::move( handler );
	}

	auto agent_loop::execute( const tool_call& call ) -> tool_outcome {
		const auto started = std::chrono::steady_clock::now( );

		auto outcome = tool_outcome{ };

		const auto finish = [&]( ) {
			outcome.elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ) - started );

			return outcome;
		};

		{
			auto payload = std::string{ "{\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"args\":";
			payload += call.args_json.empty( ) ? "{}" : call.args_json;
			payload += "}";

			log_.append( "tool.call", std::move( payload ) );
		}

		if ( budget_.exhausted( ) ) {
			outcome.ok = false;
			outcome.code = errc::budget_exhausted;
			outcome.error_message = "session budget exhausted";
			log_.append( "tool.result", "{\"ok\":false,\"error\":\"budget_exhausted\"}" );

			return finish( );
		}

		const auto* definition = registry_.find( call.name );

		if ( definition == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "unknown tool: " + call.name;

			auto payload = std::string{ "{\"ok\":false,\"error\":\"unknown_tool\",\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );

			return finish( );
		}

		const auto found = handlers_.find( call.name );
		const auto* handler = found != handlers_.end( ) ? &found->second : nullptr;

		if ( handler == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "tool has no handler registered: " + call.name;
			log_.append( "tool.result", "{\"ok\":false,\"error\":\"no_handler\"}" );

			return finish( );
		}

		// A tool handler is third-party code -- an extension's closure, a subprocess
		// wrapper -- so it can throw. Dispatch is noexcept at this boundary for the
		// same reason the event bus is: the alternative is an exception escaping into
		// the loop and a session log with a tool.call that never gets its
		// tool.result, which corrupts replay.
		auto produced = result< std::string >{ std::unexpected( fail( errc::tool_failed,
			"tool handler threw" ) ) };

		try {
			produced = ( *handler )( call.args_json );
		} catch ( const std::exception& error ) {
			produced = std::unexpected( fail( errc::tool_failed,
				std::string{ "tool handler threw: " } + error.what( ) ) );
		} catch ( ... ) {
			produced = std::unexpected( fail( errc::tool_failed,
				"tool handler threw a non-standard exception" ) );
		}

		budget_.charge( 0, 0.0 );

		if ( !produced ) {
			outcome.ok = false;
			outcome.code = produced.error( ).code;
			outcome.error_message = produced.error( ).msg;

			auto payload = std::string{ "{\"ok\":false,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"error\":\"";
			json::append_escaped( payload, produced.error( ).msg );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );

			return finish( );
		}

		outcome.ok = true;
		outcome.content = *produced;

		{
			auto payload = std::string{ "{\"ok\":true,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"source\":\"";
			payload += to_string( definition->source );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );
		}

		return finish( );
	}

}
