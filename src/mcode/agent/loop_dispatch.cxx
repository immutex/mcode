#include "mcode/agent/loop.hxx"

#include "mcode/agent/loop_internal.hxx"
#include "mcode/agent/message_json.hxx"

#include <string>
#include <utility>
#include <vector>

#include "mcode/support/json.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode {

	auto agent_loop::observe_result( const tool_call& call, const tool_outcome& outcome ) -> void {
		auto message = model::message{ };
		message.speaker = model::role::tool;
		message.blocks.push_back( loop_internal::result_block( outcome, call.id ) );

		history_.push_back( std::move( message ) );

		auto payload = std::string{ "{\"tool\":\"" };
		json::append_escaped( payload, call.name );
		payload += "\",\"ok\":";
		payload += outcome.ok ? "true" : "false";

		// A failure recorded as nothing but `ok:false` is undiagnosable after the
		// fact: the reason lived only in the message the model saw. The record
		// carries the reason, the stable class, and how long the call took.
		if ( !outcome.ok ) {
			payload += ",\"error\":\"";
			json::append_escaped( payload, outcome.error_message );
			payload += "\"";

			if ( !outcome.failure_class.empty( ) ) {
				payload += ",\"failure_class\":\"";
				json::append_escaped( payload, outcome.failure_class );
				payload += "\"";
			}

			if ( outcome.permission_denied ) {
				payload += ",\"permission_denied\":true";
			}
		}

		payload += ",\"elapsed_ms\":";
		payload += std::to_string( outcome.elapsed.count( ) );
		payload += "}";

		log_->append( "tool.result", std::move( payload ) );
	}

	auto agent_loop::request_and_fold( const model::effort effort ) -> result< bool > {
		const auto near_budget = budget_.nearly_exhausted( );
		const auto assembled = assemble_request( *registry_,
			{ .system_prompt = build_system_prompt( *registry_, instruction_chain_,
				  skill_index_ ),
				.history = history_,
				.model_name = model_name_,
				.mode = caps_.caching,
				.near_budget = near_budget,
				.recitation = { },
				.fields = provider_.request,
				.strict_tools = caps_.supports_strict_schema && provider_.features.strict_tools,
				.response_format_with_tools =
					provider_.features.response_format_with_tools } );

		auto stream_request = model::stream_request{ };
		stream_request.request = assembled.request;

		// Parallel calls are default-off for an unknown gateway: several do not implement the
		// field, and the loop serializes calls anyway, so naming it buys nothing unless the
		// descriptor says the gateway honours it.
		if ( provider_.features.parallel_tool_calls &&
			!provider_.request.parallel_tool_calls.empty( ) ) {
			stream_request.request.parallel_tool_calls = false;
		}

		// Prefill suppresses the prose preamble some models emit before a tool call. The text
		// is the gateway's convention, so it is declared in the descriptor and never guessed;
		// a gateway that rejects it is caught by the 400 downgrade path.
		if ( !provider_.features.prefill_text.empty( ) &&
			!stream_request.request.tools.empty( ) ) {
			stream_request.request.prefill = provider_.features.prefill_text;
		}
		stream_request.request.reasoning_effort = effort;
		stream_request.provider = provider_;
		stream_request.api_key = api_key_;
		stream_request.caps = caps_;

		auto events = std::vector< model::chat_event >{ };
		auto text = std::string{ };
		auto reasoning = std::string{ };

		const auto streamed = client_->stream( stream_request,
			[ & ]( const model::chat_event& event ) {
				events.push_back( event );

				if ( event.type == model::chat_event::kind::text_delta ) {
					text += event.text;

					auto payload = std::string{ "{\"text\":\"" };
					json::append_escaped( payload, event.text );
					payload += "\"}";

					publish( events::kind::assistant_delta, std::move( payload ) );
				} else if ( event.type == model::chat_event::kind::thinking_delta ) {
					reasoning += event.text;

					auto payload = std::string{ "{\"text\":\"" };
					json::append_escaped( payload, event.text );
					payload += "\"}";

					publish( events::kind::assistant_thinking, std::move( payload ) );
				}
			} );

		if ( !streamed ) {
			return std::unexpected( streamed.error( ) );
		}

		auto folded = model::usage{ };
		double cost = 0.0;

		for ( const auto& event : events ) {
			if ( event.type == model::chat_event::kind::usage ) {
				folded.add( event );
			}
		}

		// Accumulated across the run, not just this request, so the summary can report the
		// cache hit rate a consumer needs to tell a cheap turn from an expensive one.
		run_usage_ += folded;

		cost = compute_cost( caps_, folded );
		budget_.charge( static_cast< std::uint64_t >( total_tokens( caps_, folded ) ), cost );

		// Per request, not just per run. A run total cannot show a cache that only
		// starts hitting after the first call, and it cannot show a request whose
		// prefix changed. Both are the failures a cache optimization is aimed at,
		// and neither is visible in an aggregate.
		if ( log_ != nullptr ) {
			auto recorded = std::string{ "{\"input\":" };
			recorded += std::to_string( folded.input );
			recorded += ",\"output\":";
			recorded += std::to_string( folded.output );
			recorded += ",\"cached_read\":";
			recorded += std::to_string( folded.cached_read );
			recorded += ",\"cache_write\":";
			recorded += std::to_string( folded.cache_write );
			recorded += ",\"reasoning\":";
			recorded += std::to_string( folded.reasoning );
			recorded += ",\"cost_usd\":";
			recorded += std::to_string( cost );
			recorded += ",\"effort\":";
			recorded += std::to_string( static_cast< int >( effort ) );
			recorded += ",\"messages\":";
			recorded += std::to_string( history_.size( ) );
			recorded += '}';

			log_->append( std::string{ agent::MODEL_USAGE_EVENT }, std::move( recorded ) );
		}

		auto assistant = model::message{ };
		assistant.speaker = model::role::assistant;

		// Kept before the local is moved out: the repetition guard reads the
		// prose of this response, and the whole message is discarded if it turns
		// out to be a loop.
		last_response_text_ = text;

		if ( !text.empty( ) ) {
			auto block = model::block{ };
			block.kind = model::block_kind::text;
			block.text = std::move( text );
			assistant.blocks.push_back( std::move( block ) );
		}

		pending_calls_ = loop_internal::collect_calls( events );

		// A response cut off by the output limit is a distinct failure from a malformed one: the
		// arguments are a valid prefix of a call that was never finished, and no repair can
		// recover them. Recorded here so dispatch reports it as truncation, not as a parse error.
		const auto stop_reason = loop_internal::turn_stop_reason( events );

		turn_truncated_ = loop_internal::is_truncation_stop_reason( stop_reason );
		stop_reason_ = stop_reason;

		const auto& calls = pending_calls_;

		if ( turn_truncated_ ) {
			for ( auto& call : pending_calls_ ) {
				call.truncated = true;
			}
		}

		for ( const auto& call : calls ) {
			auto block = model::block{ };
			block.kind = model::block_kind::tool_call;
			block.tool_call_id = call.id;
			block.tool_name = call.name;
			block.args_json = call.args_json;
			assistant.blocks.push_back( std::move( block ) );
		}

		history_.push_back( std::move( assistant ) );

		if ( log_ != nullptr ) {
			// The reasoning is recorded before the message, so a reader sees what
			// the model was thinking when it chose the call that follows.
			if ( !reasoning.empty( ) ) {
				auto recorded = std::string{ "{\"text\":\"" };
				json::append_escaped( recorded, reasoning );
				recorded += "\"}";

				log_->append( std::string{ agent::MESSAGE_THINKING_EVENT },
					std::move( recorded ) );
			}

			log_->append( std::string{ agent::MESSAGE_ASSISTANT_EVENT }, agent::message_to_json( history_.back( ) ) );
		}

		return !calls.empty( );
	}

	// A response that repeated itself is discarded and re-asked with a corrective
	// steer, rather than finishing the run. Stopping on a thinking loop throws
	// away a task the model was usually one step from completing; the loop is a
	// symptom, and a short action-oriented nudge breaks it far more cheaply than
	// a restart.
	//
	// The steer budget is hard. Once it is spent this returns false and the
	// caller takes its normal path, so the guard cannot itself become a loop.
	auto agent_loop::steer_repetition( const agent::repetition_verdict& verdict ) -> bool {
		if ( repetition_steers_ >= agent::MAX_REPETITION_STEERS ) {
			return false;
		}

		++repetition_steers_;

		// The looping assistant message is dropped, not kept: it is the exact
		// context that caused the loop, and re-sending it invites the same
		// output. This is the one place the harness removes history it produced.
		if ( !history_.empty( ) && history_.back( ).speaker == model::role::assistant ) {
			history_.pop_back( );
		}

		// Its tool calls go with it. `request_and_fold` records both the message
		// and the calls it carried, so dropping only the message leaves the calls
		// pending -- and Act dispatches them on the next iteration, running tools
		// the model asked for in the response the harness just discarded. Their
		// results then land in the transcript as orphan blocks with no call to
		// answer, which is a transcript no provider will accept.
		pending_calls_.clear( );

		auto steer = model::message{ };
		steer.speaker = model::role::user;

		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = std::string{ agent::REPETITION_STEER };
		steer.blocks.push_back( std::move( block ) );

		history_.push_back( std::move( steer ) );

		auto payload = std::string{ "{\"repeated_characters\":" };
		payload += std::to_string( verdict.repeated_characters );
		payload += ",\"steer\":";
		payload += std::to_string( repetition_steers_ );
		payload += ",\"of\":";
		payload += std::to_string( agent::MAX_REPETITION_STEERS );
		payload += "}";

		publish( events::kind::assistant_thinking, std::move( payload ) );

		return true;
	}

	auto agent_loop::dispatch_pending( ) -> void {
		const auto pending = pending_calls_;
		pending_calls_.clear( );
		hard_error_ = dispatch_calls( pending );
	}

	// A response whose tool arguments will not parse is, on most gateways, sampling noise
	// rather than a model that cannot express the call. Blind resampling beats showing the
	// model its own broken output, which anchors a small model to that output. Truncation is
	// excluded: the same input reproduces it, so it takes the error path instead, where the
	// message can say the response was cut off. Retries saturate by the third attempt.
	auto agent_loop::resample_pending_calls( ) -> bool {
		if ( pending_calls_.empty( ) || tool_call_retries_ >= MAX_TOOL_CALL_RETRIES ) {
			return false;
		}

		auto unparsable = false;

		for ( const auto& call : pending_calls_ ) {
			if ( call.truncated ) {
				return false;
			}

			const auto* definition = registry_->find( call.name );

			if ( definition == nullptr ) {
				continue;
			}

			const auto prepared =
				tools::prepare_arguments( definition->schema_json, call.args_json );

			if ( !prepared.ok( ) ) {
				unparsable = true;

				break;
			}
		}

		if ( !unparsable ) {
			return false;
		}

		++tool_call_retries_;

		// Blind: the failed attempt leaves the conversation, so the retry is a fresh sample.
		if ( !history_.empty( ) ) {
			history_.pop_back( );
		}

		pending_calls_.clear( );

		return true;
	}

	auto agent_loop::dispatch_calls( const std::vector< tool_call >& calls ) -> bool {
		auto hard_error = false;

		for ( const auto& call : calls ) {
			// recorded first, so the repeat count decides before the call runs.
			const auto repeats = thrash_.record( call.name, call.args_json );

			auto outcome = repeats >= DOOM_LOOP_THRESHOLD
				? refuse_doom_loop( call, repeats )
				: execute( call );

			observe_result( call, outcome );

			if ( !outcome.ok ) {
				hard_error = true;
				last_failure_ = outcome.error_message;

				// The reflect ladder groups failures by class so it does not reflect twice on
				// the same problem. The message is written for the model and may embed a value
				// that changes each attempt (a repeat count), which would make every failure
				// its own class and defeat the cap; a stable class keeps the grouping honest.
				last_failure_class_ = outcome.failure_class;
			}
		}

		return hard_error;
	}

}
