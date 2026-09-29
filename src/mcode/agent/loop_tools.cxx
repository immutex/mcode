#include "mcode/agent/loop.hxx"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "mcode/support/json.hxx"

namespace mcode {

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

			log_->append( "tool.call", std::move( payload ) );
		}

		if ( budget_.exhausted( ) ) {
			outcome.ok = false;
			outcome.code = errc::budget_exhausted;
			outcome.error_message = "session budget exhausted";
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"budget_exhausted\"}" );

			return finish( );
		}

		const auto* definition = registry_->find( call.name );

		if ( definition == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "unknown tool: " + call.name;

			auto payload = std::string{ "{\"ok\":false,\"error\":\"unknown_tool\",\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\"}";

			log_->append( "tool.result", std::move( payload ) );

			return finish( );
		}

		const auto found = handlers_.find( call.name );
		const auto* handler = found != handlers_.end( ) ? &found->second : nullptr;

		if ( handler == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "tool has no handler registered: " + call.name;
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"no_handler\"}" );

			return finish( );
		}

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

			log_->append( "tool.result", std::move( payload ) );

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

			log_->append( "tool.result", std::move( payload ) );
		}

		return finish( );
	}

}
