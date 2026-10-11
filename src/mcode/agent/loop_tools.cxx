#include "mcode/agent/loop.hxx"

#include "mcode/agent/message_json.hxx"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "mcode/fs/snapshot.hxx"
#include "mcode/perm/argv.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode {

	namespace {

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

	auto agent_loop::capture_before_write( const tool_call& call, const tool_class klass ) -> void {
		if ( snapshots_ == nullptr || klass != tool_class::write ) {
			return;
		}

		const auto parsed = json::document::parse(
			call.args_json.empty( ) ? std::string_view{ "{}" } :
			std::string_view{ call.args_json } );

		if ( !parsed ) {
			log_->append( "snapshot.capture",
				tools::loop_failure( call.name, "arguments are not JSON" ) );

			return;
		}

		const auto path = parsed->pointer_string( "/path" );

		if ( !path ) {
			log_->append( "snapshot.capture", tools::loop_failure( call.name, "no path argument" ) );

			return;
		}

		auto absolute = std::filesystem::path{ *path };

		if ( absolute.is_relative( ) ) {
			absolute = std::filesystem::path{ workspace_root_ } / absolute;
		}

		const auto captured = snapshots_->capture( workspace_root_, absolute, snapshot_run_id_ );

		if ( captured ) {
			// The pinned block names what this run changed, so a compaction keeps it.
			const auto recorded = std::filesystem::relative( absolute, workspace_root_ )
				.generic_string( );

			if ( std::ranges::find( modified_files_, recorded ) == modified_files_.end( ) ) {
				modified_files_.push_back( recorded );
			}

			return;
		}

		auto payload = std::string{ "{\"ok\":false,\"tool\":\"" };
		json::append_escaped( payload, call.name );
		payload += "\",\"path\":\"";
		json::append_escaped( payload, *path );
		payload += "\",\"error\":\"";
		json::append_escaped( payload, captured.error( ).msg );
		payload += "\"}";

		log_->append( "snapshot.capture", std::move( payload ) );
	}

	auto agent_loop::refuse_doom_loop( const tool_call& call, const std::size_t repeats )
		-> tool_outcome {
		auto outcome = tool_outcome{ };

		outcome.ok = false;
		outcome.code = errc::tool_failed;
		outcome.failure_class = "doom_loop";

		const auto prior_dispatches = std::to_string( repeats - 1 );

		outcome.error_message = tools::error_result(
			"doom loop: '" + call.name + "' has already been dispatched " + prior_dispatches +
				" times with identical arguments, so this call was refused",
			"change the arguments or the approach, or report the blocker, instead of issuing "
			"the identical call again",
			true );

		auto payload = tools::loop_failure( call.name, "doom_loop" );

		log_->append( "tool.result", payload );
		publish( events::kind::tool_result, std::move( payload ) );

		return outcome;
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
			auto payload = tools::loop_failure( call.name, "budget_exhausted" );

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		const auto* definition = registry_->find( call.name );

		if ( definition == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "unknown tool: " + call.name;

			auto payload = tools::loop_failure( call.name, "unknown_tool" );

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
			auto payload = tools::loop_failure( call.name, "no_handler" );

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		// One checkpoint for every tool, built-in, extension and MCP alike: repair the
		// payload the model produced, then check it against the schema the model was shown.
		// Ahead of the veto and the permission check, so both see what will actually run.
		auto prepared = tools::prepare_arguments( definition->schema_json, call.args_json );

		if ( !prepared.ok( ) ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			// A cut-off response needs a smaller call re-issued, not a corrected parameter.
			outcome.error_message = call.truncated
				? tools::truncation_error( call.name )
				: prepared.failure;

			auto payload = tools::loop_failure( call.name,
				call.truncated ? "truncated_arguments" : "invalid_arguments" );

			log_->append( "tool.result", payload );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		const auto effective_call =
			tool_call{ call.id, call.name, prepared.json, call.truncated };

		// A veto denies even under --yolo, so it is published ahead of the handler.
		{
			auto pre_payload = std::string{ "{\"tool\":\"" };
			json::append_escaped( pre_payload, effective_call.name );
			pre_payload += "\",\"args\":";
			pre_payload += effective_call.args_json.empty( ) ? "{}" : effective_call.args_json;
			pre_payload += "}";

			auto pre_event = events::event{ };
			pre_event.type = events::kind::tool_pre_call;
			pre_event.timestamp_ms = support::epoch_milliseconds( );
			pre_event.payload_json = std::move( pre_payload );

			const auto veto = bus_ != nullptr ? bus_->publish( std::move( pre_event ) )
				: std::optional< events::veto >{ };

			if ( veto ) {
				outcome.ok = false;
				outcome.code = errc::tool_failed;
				outcome.error_message = "denied by extension veto: " +
					( veto->reason.empty( ) ? std::string{ "policy" } : veto->reason );
				outcome.permission_denied = true;
				permission_denied_ = true;

				auto payload = tools::loop_denial( { .tool_name = call.name, .veto = true,
					.detail_key = "source", .detail = veto->source, .reason = veto->reason } );

				log_->append( "tool.result", payload );
				publish( events::kind::tool_result, std::move( payload ) );

				return finish( );
			}
		}

		// One check point for built-in, extension and MCP tools alike, skipped for a
		// harness-owned call: the verification command comes from configuration, not
		// from the model, so the agent's approval policy does not apply to it.
		if ( permissions_ != nullptr && !harness_call_ ) {
			auto request = perm::permission_request{ };
			request.tool_name = effective_call.name;
			request.klass = definition->klass;
			request.owner = definition->owner;
			request.resource = permission_resource( effective_call, definition->klass );

			const auto decision = permissions_->decide( request );

			if ( decision == perm::permission_decision::deny ) {
				const auto& verdict = permissions_->last_verdict( );

				outcome.ok = false;
				outcome.code = errc::tool_failed;
				outcome.error_message = "denied by the permission engine: " +
					( verdict.reason.empty( ) ? std::string{ "policy" } : verdict.reason );
				outcome.permission_denied = true;
				permission_denied_ = true;

				auto rule = verdict.matched.scope + ": " + verdict.matched.pattern;

				auto payload = tools::loop_denial( { .tool_name = call.name,
					.detail_key = "rule", .detail = rule, .reason = verdict.reason } );

				log_->append( "tool.result", payload );
				publish( events::kind::tool_result, std::move( payload ) );

				return finish( );
			}
		}

		// After the permission check, so a refused call never fills the store, and ahead
		// of the handler, so the bytes recorded are the ones about to be replaced.
		capture_before_write( effective_call, definition->klass );

		// The audit trail names the expanded command that runs, not just its output.
		if ( definition->klass == tool_class::exec ) {
			auto command = json::document::parse( effective_call.args_json );

			if ( command ) {
				if ( auto text = command->pointer_string( "/command" ) ) {
					auto payload = std::string{ "{\"tool\":\"" };
					json::append_escaped( payload, effective_call.name );
					payload += "\",\"command\":\"";
					json::append_escaped( payload, *text );
					payload += "\"}";

					log_->append( "tool.command", std::move( payload ) );
				}
			}
		}

		auto produced = result< std::string >{ std::unexpected( fail( errc::tool_failed,
			"tool handler threw" ) ) };

		try {
			produced = ( *handler )( prepared.json );
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

			// Logged under the same kind as a success, and with the call id, which is what
			// ties a result to its call.
			auto recorded = std::string{ "{\"ok\":false,\"tool\":\"" };
			json::append_escaped( recorded, call.name );
			recorded += "\",\"call_id\":\"";
			json::append_escaped( recorded, call.id );
			recorded += "\",\"content\":\"";
			json::append_escaped( recorded, produced.error( ).msg );
			recorded += "\"}";

			auto payload = tools::loop_failure( call.name, produced.error( ).msg );

			log_->append( std::string{ agent::TOOL_RESULT_EVENT }, std::move( recorded ) );
			publish( events::kind::tool_result, std::move( payload ) );

			return finish( );
		}

		outcome.ok = true;
		outcome.content = *produced;

		// The flag must not survive a success, or a recovered run would still exit 5.
		permission_denied_ = false;

		{
			// The stream payload omits the output, which is unbounded.
			auto recorded = std::string{ "{\"ok\":true,\"tool\":\"" };
			json::append_escaped( recorded, call.name );
			recorded += "\",\"call_id\":\"";
			json::append_escaped( recorded, call.id );
			recorded += "\",\"source\":\"";
			recorded += to_string( definition->source );
			recorded += "\",\"content\":\"";
			json::append_escaped( recorded, outcome.content );
			recorded += "\"}";

			auto payload = std::string{ "{\"ok\":true,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"source\":\"";
			payload += to_string( definition->source );
			payload += "\"}";

			log_->append( std::string{ agent::TOOL_RESULT_EVENT }, std::move( recorded ) );
			publish( events::kind::tool_result, std::move( payload ) );
		}

		return finish( );
	}

}
