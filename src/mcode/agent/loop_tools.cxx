#include "mcode/agent/loop.hxx"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "mcode/perm/argv.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

namespace mcode {

	namespace {

		// The class comes from the registry, not the tool name: same-class tools are identical.
		[[nodiscard]] auto permission_resource( const tool_call& call,
			const tool_class klass ) -> std::string {
			if ( klass != tool_class::exec && klass != tool_class::read &&
				klass != tool_class::write ) {
				return call.args_json;
			}

			const auto parsed = json::document::parse(
				call.args_json.empty( ) ? std::string_view{ "{}" } :
				std::string_view{ call.args_json } );

			if ( !parsed ) {
				return { };
			}

			if ( klass != tool_class::exec ) {
				// The path, not the blob: raw args JSON would look like a write outside the root.
				const auto path = parsed->pointer_string( "/path" );

				return path ? *path : std::string{ };
			}

			const auto command = parsed->pointer_string( "/command" );

			if ( !command ) {
				return { };
			}

			const auto tokens = perm::parse_command_line( *command );

			if ( !tokens ) {
				// The empty resource makes the default exec rule an ask, never an allow.
				return { };
			}

			return perm::canonical_argv( *tokens );
		}

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

			log_->append( "tool.call", payload );
			publish( events::kind::tool_call, std::move( payload ) );
		}

		if ( budget_.exhausted( ) ) {
			outcome.ok = false;
			outcome.code = errc::budget_exhausted;
			outcome.error_message = "session budget exhausted";
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"budget_exhausted\"}" );
			publish( events::kind::tool_result, "{\"ok\":false,\"error\":\"budget_exhausted\"}" );

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

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		const auto found = handlers_.find( call.name );
		const auto* handler = found != handlers_.end( ) ? &found->second : nullptr;

		if ( handler == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "tool has no handler registered: " + call.name;
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"no_handler\"}" );
			publish( events::kind::tool_result, "{\"ok\":false,\"error\":\"no_handler\"}" );

			return finish( );
		}

		// A veto denies even under --yolo, so it is published before the handler runs.
		{
			auto pre_payload = std::string{ "{\"tool\":\"" };
			json::append_escaped( pre_payload, call.name );
			pre_payload += "\",\"args\":";
			pre_payload += call.args_json.empty( ) ? "{}" : call.args_json;
			pre_payload += "}";

			auto pre_event = events::event{ };
			pre_event.type = events::kind::tool_pre_call;
			pre_event.timestamp_ms = support::epoch_milliseconds( );
			pre_event.payload_json = std::move( pre_payload );

			// The returned veto is the decision, not a notification.
			const auto veto = bus_ != nullptr ? bus_->publish( std::move( pre_event ) )
				: std::optional< events::veto >{ };

			if ( veto ) {
				outcome.ok = false;
				outcome.code = errc::tool_failed;
				outcome.error_message = "denied by extension veto: " +
					( veto->reason.empty( ) ? std::string{ "policy" } : veto->reason );
				outcome.permission_denied = true;
				permission_denied_ = true;

				auto payload = std::string{ "{\"ok\":false,\"denied\":true,\"veto\":true,\"tool\":\"" };
				json::append_escaped( payload, call.name );
				payload += "\",\"source\":\"";
				json::append_escaped( payload, veto->source );
				payload += "\",\"reason\":\"";
				json::append_escaped( payload, veto->reason );
				payload += "\"}";

				log_->append( "tool.result", payload );
				publish( events::kind::tool_result, std::move( payload ) );

				return finish( );
			}
		}

		// One check point: built-in, extension and MCP tools all pass through here.
		if ( permissions_ != nullptr ) {
			auto request = perm::permission_request{ };
			request.tool_name = call.name;
			request.klass = definition->klass;
			request.owner = definition->owner;
			request.resource = permission_resource( call, definition->klass );

			const auto decision = permissions_->decide( request );

			if ( decision == perm::permission_decision::deny ) {
				const auto& verdict = permissions_->last_verdict( );

				outcome.ok = false;
				outcome.code = errc::tool_failed;
				outcome.error_message = "denied by the permission engine: " +
					( verdict.reason.empty( ) ? std::string{ "policy" } : verdict.reason );
				outcome.permission_denied = true;
				permission_denied_ = true;

				auto payload = std::string{ "{\"ok\":false,\"denied\":true,\"tool\":\"" };
				json::append_escaped( payload, call.name );
				payload += "\",\"rule\":\"";
				json::append_escaped( payload, verdict.matched.scope + ": " +
					verdict.matched.pattern );
				payload += "\",\"reason\":\"";
				json::append_escaped( payload, verdict.reason );
				payload += "\"}";

				log_->append( "tool.result", payload );
				publish( events::kind::tool_result, std::move( payload ) );

				return finish( );
			}
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

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		outcome.ok = true;
		outcome.content = *produced;

		// The flag must not survive a success, or a recovered run would still exit 5.
		permission_denied_ = false;

		{
			auto payload = std::string{ "{\"ok\":true,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"source\":\"";
			payload += to_string( definition->source );
			payload += "\"}";

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );
		}

		return finish( );
	}

}
