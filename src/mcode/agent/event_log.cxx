#include "mcode/agent/loop.hxx"

#include <algorithm>
#include <fstream>

#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

namespace mcode {

	auto event::to_json( ) const -> std::string {
		auto out = std::string{ "{\"v\":" };
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
		, sink_( std::move( other.sink_ ) )
		, path_( std::move( other.path_ ) )
		, write_failures_( other.write_failures_ ) {
		other.sink_ = nullptr;
	}

	auto event_log::operator=( event_log&& other ) noexcept -> event_log& {
		if ( this == &other ) {
			return *this;
		}

		close( );

		events_ = std::move( other.events_ );
		next_sequence_ = other.next_sequence_;
		sink_ = std::move( other.sink_ );
		path_ = std::move( other.path_ );
		write_failures_ = other.write_failures_;

		other.sink_ = nullptr;

		return *this;
	}

	auto event_log::open( const std::filesystem::path& path ) -> status {
		close( );

#ifdef _WIN32
		auto* file = _wfopen( path.c_str( ), L"ab" );
#else
		auto* file = std::fopen( path.string( ).c_str( ), "ab" );
#endif

		if ( file == nullptr ) {
			return std::unexpected( fail( errc::io, "could not open session log: " + path.string( ) ) );
		}

		sink_ = decltype( sink_ ){ file, []( std::FILE* handle ) { std::fclose( handle ); } };
		path_ = path;

		if ( std::fseek( file, 0, SEEK_END ) != 0 ) {
			return std::unexpected( fail( errc::io, "could not seek session log" ) );
		}

		const auto size = std::ftell( file );

		if ( size == 0 ) {
			next_sequence_ = 0;
			events_.clear( );

			return status{ };
		}

		auto replayed = replay_event_log( path );

		if ( !replayed ) {
			sink_ = nullptr;

			return std::unexpected( replayed.error( ) );
		}

		for ( auto& restored : replayed->log.events_ ) {
			events_.push_back( std::move( restored ) );
		}

		next_sequence_ = replayed->log.next_sequence_;

		if ( replayed->truncated_tail ) {
			++write_failures_;
		}

		return status{ };
	}

	auto event_log::close( ) -> void {
		if ( sink_ != nullptr ) {
			std::fflush( sink_.get( ) );
			sink_ = nullptr;
		}
	}

	auto event_log::append( std::string kind, std::string payload_json ) -> event {
		// A payload that does not parse makes the line unreadable to every
		// consumer, and it fails silently: the run looks healthy and the record
		// is garbage. The harness builds these strings by hand, so a malformed
		// one is a programming error, and this is the single choke point every
		// payload passes through. The original bytes are preserved inside a
		// valid envelope so a forensic reader still sees them.
		if ( !payload_json.empty( ) && !json::document::parse( payload_json ) ) {
			auto wrapped = std::string{ "{\"malformed_payload\":true,\"kind\":\"" };
			json::append_escaped( wrapped, kind );
			wrapped += "\",\"raw\":\"";
			json::append_escaped( wrapped, payload_json );
			wrapped += "\"}";

			++write_failures_;
			payload_json = std::move( wrapped );
		}

		auto recorded = event{ };
		recorded.version = 1;
		recorded.sequence = next_sequence_++;
		recorded.timestamp_ms = support::epoch_milliseconds( );
		recorded.kind = std::move( kind );
		recorded.payload_json = std::move( payload_json );

		events_.push_back( recorded );

		if ( sink_ != nullptr ) {
			auto line = recorded.to_json( );
			line += '\n';

			if ( std::fwrite( line.data( ), 1, line.size( ), sink_.get( ) ) != line.size( ) ||
				std::fflush( sink_.get( ) ) != 0 ) {
				++write_failures_;
			}
		}

		return recorded;
	}

	auto event_log::restore( event recorded ) -> void {
		next_sequence_ = std::max( next_sequence_, recorded.sequence + 1 );
		events_.push_back( std::move( recorded ) );
	}

	auto event_log::to_jsonl( ) const -> std::string {
		auto out = std::string{ };

		for ( const auto& value : events_ ) {
			out += value.to_json( );
			out += '\n';
		}

		return out;
	}

	auto replay_event_log( const std::filesystem::path& path ) -> result< replay_result > {
		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "could not open session log: " + path.string( ) ) );
		}

		auto out = replay_result{ };
		auto line = std::string{ };

		while ( std::getline( stream, line ) ) {
			if ( stream.eof( ) && !line.empty( ) ) {
				out.truncated_tail = true;

				break;
			}

			if ( line.empty( ) ) {
				continue;
			}

			auto parsed = json::document::parse( line );

			if ( !parsed ) {
				++out.malformed_lines;

				continue;
			}

			auto restored = event{ };
			auto number = parsed->get_int( "v" );

			if ( number ) {
				restored.version = static_cast< std::uint32_t >( *number );
			}

			if ( auto value = parsed->get_int( "seq" ) ) {
				restored.sequence = static_cast< std::uint64_t >( *value );
			}

			if ( auto value = parsed->get_int( "ts" ) ) {
				restored.timestamp_ms = *value;
			}

			if ( auto value = parsed->get_string( "kind" ) ) {
				restored.kind = *value;
			}

			if ( auto value = parsed->get_string( "run" ) ) {
				restored.run = *value;
			}

			if ( auto value = parsed->get_int( "turn" ) ) {
				restored.turn = static_cast< std::uint32_t >( *value );
			}

			if ( auto value = parsed->get_int( "step" ) ) {
				restored.step = static_cast< std::uint32_t >( *value );
			}

			if ( auto value = parsed->pointer_raw( "/payload" ) ) {
				restored.payload_json = *value;
			}

			out.log.restore( std::move( restored ) );
			++out.events_read;
		}

		if ( stream.bad( ) ) {
			return std::unexpected( fail( errc::io, "read failed on session log" ) );
		}

		return out;
	}

}
