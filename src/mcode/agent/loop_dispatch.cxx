#include "mcode/agent/loop.hxx"

#include "mcode/agent/loop_internal.hxx"
#include "mcode/agent/message_json.hxx"

#include <string>
#include <utility>
#include <vector>

#include "mcode/support/json.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode {

	auto agent_loop::observe_result( const tool_call& call, const tool_outcome& outcome ) -> void {
		// An MCP result is attacker-controlled input, so marking it here makes the
		// taint gate a fact about this session.
		if ( permissions_ != nullptr ) {
			if ( const auto* definition = registry_->find( call.name );
				definition != nullptr && definition->source == tool_source::mcp ) {
				permissions_->mark_untrusted_ingested( );
			}
		}

		auto message = model::message{ };
		message.speaker = model::role::tool;
		message.blocks.push_back( loop_internal::result_block( outcome, call.id ) );

		history_.push_back( std::move( message ) );

		auto payload = std::string{ "{\"tool\":\"" };
		json::append_escaped( payload, call.name );
		payload += "\",\"ok\":";
		payload += outcome.ok ? "true" : "false";

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

	auto agent_loop::request_and_fold( const model::effort effort, const model_tier tier )
		-> result< bool > {
		const auto& routed = tier_for( tier );
		const auto near_budget = budget_.nearly_exhausted( );
		const auto assembled = assemble_request( *registry_,
			{ .system_prompt = build_system_prompt( *registry_, instruction_chain_,
				  skill_index_, notes_pointer_ ),
				.history = history_,
				.model_name = routed.name,
				.mode = routed.caps.caching,
				.near_budget = near_budget,
				.pinned_state = render_pinned_state( pinned_ ),
				.fields = provider_.request,
				.strict_tools =
					routed.caps.supports_strict_schema && provider_.features.strict_tools,
				.response_format_with_tools =
					provider_.features.response_format_with_tools } );

		auto stream_request = model::stream_request{ };
		stream_request.request = assembled.request;

		// Default-off for an unknown gateway: several do not implement the field.
		if ( provider_.features.parallel_tool_calls &&
			!provider_.request.parallel_tool_calls.empty( ) ) {
			stream_request.request.parallel_tool_calls = false;
		}

		if ( !provider_.features.prefill_text.empty( ) &&
			!stream_request.request.tools.empty( ) ) {
			stream_request.request.prefill = provider_.features.prefill_text;
		}
		stream_request.request.reasoning_effort = effort;
		stream_request.provider = provider_;
		stream_request.api_key = api_key_;
		stream_request.caps = routed.caps;

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

		run_usage_ += folded;

		// The last request's prompt, not the run's spend, so the meter stays a
		// fraction of the window.
		occupancy_.prompt_tokens = folded.input + folded.cached_read + folded.cache_write;
		occupancy_.cached_read = folded.cached_read;
		occupancy_.cache_write = folded.cache_write;
		occupancy_.usable_window = static_cast< std::int64_t >(
			std::max< std::int64_t >(
				routed.caps.effective_context_window( ) - RESERVED_OUTPUT_TOKENS,
				static_cast< std::int64_t >( static_cast< double >(
					routed.caps.effective_context_window( ) ) * MIN_USABLE_WINDOW_FRACTION ) ) );

		cost = compute_cost( routed.caps, folded );
		budget_.charge( static_cast< std::uint64_t >( total_tokens( routed.caps, folded ) ),
			cost );

		// Per request: an aggregate cannot show a cache that only starts hitting later.
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

		last_response_text_ = text;

		if ( !text.empty( ) ) {
			auto block = model::block{ };
			block.kind = model::block_kind::text;
			block.text = std::move( text );
			assistant.blocks.push_back( std::move( block ) );
		}

		pending_calls_ = loop_internal::collect_calls( events );

		// A cut-off response is a valid prefix of a call that was never finished, so no
		// repair can recover it; recorded so dispatch reports truncation, not malformed.
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

	// Returns false once the steer budget is spent, so the guard cannot itself loop.
	auto agent_loop::steer_repetition( const agent::repetition_verdict& verdict ) -> bool {
		if ( repetition_steers_ >= agent::MAX_REPETITION_STEERS ) {
			return false;
		}

		++repetition_steers_;

		// The exact context that caused the loop, so re-sending it invites the same output.
		if ( !history_.empty( ) && history_.back( ).speaker == model::role::assistant ) {
			history_.pop_back( );
		}

		// Dropping the message without its calls leaves Act to run tools from the
		// discarded response.
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

	// Blind resampling: showing a small model its own broken output anchors it to it.
	// Truncation is excluded, since the same input reproduces it.
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

		if ( !history_.empty( ) ) {
			history_.pop_back( );
		}

		pending_calls_.clear( );

		return true;
	}

	auto agent_loop::dispatch_calls( const std::vector< tool_call >& calls ) -> bool {
		auto hard_error = false;

		for ( const auto& call : calls ) {
			// recorded first, so the repeat count decides ahead of the call.
			const auto repeats = thrash_.record( call.name, call.args_json );

			auto outcome = repeats >= DOOM_LOOP_THRESHOLD
				? refuse_doom_loop( call, repeats )
				: execute( call );

			observe_result( call, outcome );

			if ( !outcome.ok ) {
				hard_error = true;
				last_failure_ = outcome.error_message;

				// The class is stable; the message may embed a value that changes each attempt.
				last_failure_class_ = outcome.failure_class;
			}
		}

		return hard_error;
	}

}
