#include "mcode/agent/loop.hxx"

#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mcode {

	namespace {

		[[nodiscard]] auto now_ms( ) -> std::int64_t {
			return std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::system_clock::now( ).time_since_epoch( ) ).count( );
		}

		auto append_escaped( std::string& out, const std::string_view text ) -> void {
			for ( const auto character : text ) {
				switch ( character ) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;

					default:
						if ( static_cast< unsigned char >( character ) < 0x20 ) {
							auto buffer = std::array< char, 8 >{ };
							std::snprintf( buffer.data( ), buffer.size( ), "\\u%04x",
								static_cast< unsigned char >( character ) );
							out += buffer.data( );
						} else {
							out += character;
						}
				}
			}
		}

	}

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
		append_escaped( out, kind );
		out += "\",\"run\":\"";
		append_escaped( out, run );
		out += "\",\"turn\":";
		out += std::to_string( turn );
		out += ",\"step\":";
		out += std::to_string( step );
		out += ",\"payload\":";
		out += payload_json.empty( ) ? "{}" : payload_json;
		out += "}";

		return out;
	}

	auto event_log::append( std::string kind, std::string payload_json ) -> event {
		auto appended = event{ };
		appended.sequence = next_sequence_++;
		appended.timestamp_ms = now_ms( );
		appended.kind = std::move( kind );
		appended.payload_json = std::move( payload_json );
		appended.run = branch_id_;

		events_.push_back( std::move( appended ) );

		return events_.back( );
	}

	auto event_log::to_jsonl( ) const -> std::string {
		auto out = std::string{ };

		for ( const auto& entry : events_ ) {
			out += entry.to_json( );
			out += '\n';
		}

		return out;
	}

	auto agent_loop::register_handler( std::string name, tool_handler handler ) -> void {
		for ( auto& [ existing, function ] : handlers_ ) {
			if ( existing == name ) {
				function = std::move( handler );

				return;
			}
		}

		handlers_.emplace_back( std::move( name ), std::move( handler ) );
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
			append_escaped( payload, call.name );
			payload += "\",\"args\":";
			payload += call.args_json.empty( ) ? "{}" : call.args_json;
			payload += "}";

			log_.append( "tool.call", std::move( payload ) );
		}

		if ( budget_.exhausted( ) ) {
			outcome.ok = false;
			outcome.code = errc::cancelled;
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
			append_escaped( payload, call.name );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );

			return finish( );
		}

		const auto* handler = static_cast< const tool_handler* >( nullptr );

		for ( const auto& [ name, function ] : handlers_ ) {
			if ( name == call.name ) {
				handler = &function;

				break;
			}
		}

		if ( handler == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "tool has no handler registered: " + call.name;
			log_.append( "tool.result", "{\"ok\":false,\"error\":\"no_handler\"}" );

			return finish( );
		}

		auto produced = ( *handler )( call.args_json );
		budget_.charge( 0, 0.0 );

		if ( !produced ) {
			outcome.ok = false;
			outcome.code = produced.error( ).code;
			outcome.error_message = produced.error( ).msg;

			auto payload = std::string{ "{\"ok\":false,\"tool\":\"" };
			append_escaped( payload, call.name );
			payload += "\",\"error\":\"";
			append_escaped( payload, produced.error( ).msg );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );

			return finish( );
		}

		outcome.ok = true;
		outcome.content = *produced;

		{
			auto payload = std::string{ "{\"ok\":true,\"tool\":\"" };
			append_escaped( payload, call.name );
			payload += "\",\"source\":\"";
			payload += to_string( definition->source );
			payload += "\"}";

			log_.append( "tool.result", std::move( payload ) );
		}

		return finish( );
	}

}
