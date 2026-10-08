#include "mcode/agent/loop.hxx"

#include "mcode/agent/loop_internal.hxx"
#include "mcode/agent/message_json.hxx"

#include <algorithm>
#include <atomic>
#include <exception>
#include <fstream>
#include <map>

#include "mcode/perm/permission.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/tool_args.hxx"

#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mcode {

	auto to_string( const loop_state value ) noexcept -> std::string_view {
		switch ( value ) {
			case loop_state::idle: return "idle";
			case loop_state::plan: return "plan";
			case loop_state::act: return "act";
			case loop_state::observe: return "observe";
			case loop_state::verify: return "verify";
			case loop_state::reflect: return "reflect";
			case loop_state::replan: return "replan";
			case loop_state::handoff: return "handoff";
			case loop_state::done: return "done";
			case loop_state::failed: return "failed";
		}

		return "idle";
	}

	agent_loop::agent_loop( dependencies deps )
		: registry_( deps.registry ), client_( deps.client ),
		log_( deps.log ), bus_( deps.bus ), budget_( deps.budget ),
		model_name_( deps.model_name ), caps_( deps.caps ),
		provider_( deps.provider ), api_key_( deps.api_key ),
		workspace_root_( deps.workspace_root ),
		platform_name_( deps.platform_name ),
		snapshots_( deps.snapshots ),
		permissions_( deps.permissions ),
		instruction_chain_( deps.instruction_chain ),
		skill_index_( deps.skill_index ),
		pump_timers_( std::move( deps.pump_timers ) ) {
		if ( registry_ == nullptr ) {
			registry_ = &owned_registry_;
		}

		if ( log_ == nullptr ) {
			log_ = &owned_log_;
		}

		if ( bus_ == nullptr ) {
			bus_ = &owned_bus_;
		}
	}

	auto agent_loop::mint_run_id( ) -> void {
		// The serial keeps two runs inside one millisecond from sharing a capture group.
		static auto serial = std::atomic< std::uint64_t >{ 0 };

		snapshot_run_id_ = std::to_string( support::epoch_milliseconds( ) ) + "-" +
			std::to_string( serial.fetch_add( 1, std::memory_order_relaxed ) );
	}

	auto agent_loop::publish( const events::kind type, std::string payload_json ) -> void {
		if ( bus_ == nullptr ) {
			return;
		}

		auto value = events::event{ };
		value.type = type;
		value.timestamp_ms = support::epoch_milliseconds( );
		value.payload_json = std::move( payload_json );

		bus_->publish( std::move( value ) );
	}

	auto agent_loop::register_handler( const std::string name, tool_handler handler ) -> void {
		handlers_.insert_or_assign( std::move( name ), std::move( handler ) );
	}

	auto agent_loop::observe_result( const tool_call& call, const tool_outcome& outcome ) -> void {
		auto message = model::message{ };
		message.speaker = model::role::tool;
		message.blocks.push_back( loop_internal::result_block( outcome, call.id ) );

		history_.push_back( std::move( message ) );

		auto payload = std::string{ "{\"tool\":\"" };
		json::append_escaped( payload, call.name );
		payload += "\",\"ok\":";
		payload += outcome.ok ? "true" : "false";
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

	auto agent_loop::finish_run( const loop_state terminal, const std::string_view reason )
		-> void {
		state_ = terminal;
		end_reason_ = std::string{ reason };

		auto summary = std::string{ "{\"goal\":\"" };
		json::append_escaped( summary, user_task_ );
		summary += "\",\"actions\":";
		summary += std::to_string( budget_.steps_used.load( ) );
		summary += ",\"last_failure\":\"";
		json::append_escaped( summary, last_failure_ );
		summary += "\",\"remaining_steps\":";
		summary += std::to_string( budget_.max_steps > budget_.steps_used.load( )
				? budget_.max_steps - budget_.steps_used.load( )
				: 0 );
		summary += "\",\"remaining_usd\":";
		summary += std::to_string( budget_.max_usd > budget_.usd_used.load( )
				? budget_.max_usd - budget_.usd_used.load( )
				: 0.0 );
		summary += ",\"input_tokens\":";
		summary += std::to_string( run_usage_.input );
		summary += ",\"output_tokens\":";
		summary += std::to_string( run_usage_.output );
		summary += ",\"cached_read_tokens\":";
		summary += std::to_string( run_usage_.cached_read );
		summary += ",\"cache_write_tokens\":";
		summary += std::to_string( run_usage_.cache_write );
		summary += ",\"reasoning_tokens\":";
		summary += std::to_string( run_usage_.reasoning );
		summary += ",\"state\":\"";
		summary += to_string( terminal );
		summary += "\",\"reason\":\"";
		json::append_escaped( summary, reason );
		summary += "}";

		log_->append( "run.end", summary );

		// The bus has a kind for a failed run that nothing else publishes.
		if ( terminal == loop_state::failed ) {
			publish( events::kind::error, std::move( summary ) );
		}
	}

	// A restored transcript replaces the history outright. It is not appended to:
	// a resumed session continues one conversation, and merging two would send
	// the model a transcript it never produced.
	auto agent_loop::seed_history( std::vector< model::message > restored ) -> void {
		history_ = std::move( restored );
	}

	auto agent_loop::approval_mode( ) const noexcept -> std::string_view {
		return permissions_ != nullptr ? permissions_->approval_mode( ) : std::string_view{ };
	}

	auto agent_loop::run( const std::string_view user_task ) -> result< turn_outcome > {
		user_task_ = std::string{ user_task };
		visited_.clear( );
		pending_calls_.clear( );
		plan_answered_ = false;
		hard_error_ = false;
		permission_denied_ = false;

		// the doom-loop repeat count rides on this window, so this clears it too.
		thrash_ = thrash_detector{ };
		reflection_counts_.clear( );
		total_reflections_ = 0;
		last_failure_.clear( );
		last_failure_class_.clear( );
		end_reason_.clear( );
		stop_reason_.clear( );
		turn_truncated_ = false;
		tool_call_retries_ = 0;
		run_usage_ = model::usage{ };
		replan_count_ = 0;
		last_failure_repeats_ = 0;
		previous_texts_.clear( );
		repetition_steers_ = 0;
		last_response_text_.clear( );

		// A fresh capture group per run, so /undo undoes this run and not the previous one.
		mint_run_id( );

		auto task_message = model::message{ };
		task_message.speaker = model::role::user;

		auto task_block = model::block{ };
		task_block.kind = model::block_kind::text;
		task_block.text = user_task_;
		task_message.blocks.push_back( std::move( task_block ) );

		history_.push_back( std::move( task_message ) );

		// The conversation is what makes a session resumable, so it is recorded.
		// Without this the log says a tool ran but never what was asked or
		// answered, and `--continue` restores a sequence number, not a session.
		if ( log_ != nullptr && !history_.empty( ) ) {
			log_->append( std::string{ agent::MESSAGE_USER_EVENT }, agent::message_to_json( history_.back( ) ) );
		}

		return run_state_machine( );
	}

	auto agent_loop::run_state_machine( ) -> turn_outcome {
		auto outcome = turn_outcome{ };

		state_ = loop_state::plan;

		publish( events::kind::turn_start, "{}" );

		// `turn_end` must fire on EVERY exit, including the early returns below.
		struct turn_end_guard {
			agent_loop* self;

			~turn_end_guard( ) { self->publish( events::kind::turn_end, "{}" ); }
		} const guard{ this };

		while ( true ) {
			// Checked at the step boundary, which is the honest bound: a request
			// already in flight is not abandoned mid-stream.
			if ( cancel_ != nullptr && cancel_->load( ) ) {
				finish_run( loop_state::handoff, "interrupted" );
				outcome.final_state = loop_state::handoff;
				outcome.visited = visited_;
				outcome.summary_json = "interrupted";

				return outcome;
			}

			publish( events::kind::step_start, std::string{ "{\"state\":\"" }
				+ std::string{ to_string( state_ ) } + "\"}" );

			// timers fire on this thread, once per step, never inside a hook's dispatch.
			if ( pump_timers_ ) {
				pump_timers_( );
			}

			visited_.push_back( state_ );

			switch ( state_ ) {
				case loop_state::plan: {
					if ( budget_.steps_used.load( ) >= budget_.max_steps ) {
						state_ = loop_state::failed;

						break;
					}

					auto planned = request_and_fold( model::effort::high );

					if ( !planned ) {
						finish_run( loop_state::failed, planned.error( ).msg );
						outcome.final_state = state_;
						outcome.visited = visited_;
						outcome.summary_json = planned.error( ).msg;

						return outcome;
					}

					// no tool call means this response is the answer, so Act must not re-request it.
					plan_answered_ = !*planned;
					state_ = loop_state::act;

					break;
				}

				case loop_state::act: {
					// dispatch before the budget check, so a spent step still answers its calls.
					if ( !pending_calls_.empty( ) ) {
						dispatch_pending( );
						state_ = loop_state::observe;

						break;
					}

					if ( budget_.exhausted( ) ) {
						finish_run( loop_state::handoff, "budget exhausted" );
						state_ = loop_state::handoff;

						break;
					}

					if ( plan_answered_ ) {
						plan_answered_ = false;
						state_ = loop_state::verify;

						break;
					}

					auto acted = request_and_fold( model::effort::medium );

					if ( !acted ) {
						finish_run( loop_state::handoff, acted.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					// A thinking loop is checked before the response is used. The
					// guard discards the looping message and re-asks with a short
					// steer, so the run continues instead of ending on a symptom.
					// When the steer budget is spent the verdict is ignored and the
					// response is processed normally.
					if ( const auto verdict = agent::detect_repetition( last_response_text_,
						previous_texts_ ); verdict.looped ) {
						if ( steer_repetition( verdict ) ) {
							break;
						}
					}

					if ( !last_response_text_.empty( ) ) {
						previous_texts_.push_back( last_response_text_ );
					}

					if ( *acted ) {
						// A response whose arguments cannot be repaired is resampled blind:
						// the attempt is dropped and the request repeated, because showing a
						// small model its own broken output anchors it to that output.
						if ( resample_pending_calls( ) ) {
							break;
						}

						dispatch_pending( );
						state_ = loop_state::observe;

						break;
					}

					state_ = loop_state::verify;

					break;
				}

				case loop_state::observe: {
					if ( budget_.exhausted( ) ) {
						finish_run( loop_state::handoff, "budget exhausted" );
						state_ = loop_state::handoff;

						break;
					}

					// The repeated-call ladder is consulted before the failure branch. A call the
					// dispatch guard refused is both a repeat and a failure, and the repeat is
					// the informative reading: the ladder reflects once, then demands a new
					// plan, where the failure branch alone would spend the whole reflection
					// budget on a loop only replanning can break.
					const auto repeats = thrash_.repeat_count( );

					if ( repeats >= THRASH_ESCALATION_FACTOR * THRASH_REPEAT_LIMIT ) {
						++replan_count_;

						if ( replan_count_ >= REPLAN_GUARD_LIMIT ) {
							finish_run( loop_state::handoff, "thrash beyond replan guard" );
							state_ = loop_state::handoff;

							break;
						}

						state_ = loop_state::replan;

						break;
					}

					if ( repeats >= THRASH_REPEAT_LIMIT ) {
						if ( total_reflections_ >= MAX_REFLECTIONS_PER_RUN ) {
							state_ = loop_state::replan;

							break;
						}

						state_ = loop_state::reflect;

						break;
					}

					if ( hard_error_ && total_reflections_ < MAX_REFLECTIONS_PER_RUN ) {
						hard_error_ = false;
						state_ = loop_state::reflect;

						break;
					}

					hard_error_ = false;

					state_ = loop_state::act;

					break;
				}

				case loop_state::verify: {
					if ( verification_command_.empty( ) ) {
						state_ = loop_state::handoff;

						break;
					}

					// the command is a JSON string member, so it must be quoted and escaped;
					// an unquoted value is not JSON and the call would be rejected.
					auto arguments = std::string{ "{\"command\":\"" };
					json::append_escaped( arguments, verification_command_ );
					arguments += "\"}";

					auto checked = execute( tool_call{ std::string{ }, "bash", arguments } );

					// A non-zero exit is still a successful tool call; the gate reads the code.
					auto passed = checked.ok;

					if ( passed ) {
						auto parsed = json::document::parse( checked.content );

						if ( parsed ) {
							const auto code = parsed->pointer_int( "/exit_code" );
							const auto timed_out = parsed->pointer_bool( "/timed_out" );

							passed = code.has_value( ) && *code == 0 &&
								( !timed_out.has_value( ) || !*timed_out );
						} else {
							passed = false;
						}
					}

					if ( passed ) {
						state_ = loop_state::done;

						break;
					}

					last_failure_ = checked.ok ? "verification command failed"
												: checked.error_message;

					if ( total_reflections_ >= MAX_REFLECTIONS_PER_RUN ) {
						state_ = loop_state::handoff;

						break;
					}

					state_ = loop_state::reflect;

					break;
				}

				case loop_state::reflect: {
					const auto failure_class = !last_failure_class_.empty( )
						? last_failure_class_
						: last_failure_.empty( ) ? std::string{ "thrash" } : last_failure_;

					auto& count = reflection_counts_[ failure_class ];

					if ( count >= MAX_REFLECTIONS_PER_FAILURE_CLASS ) {
						state_ = loop_state::replan;

						break;
					}

					++count;
					++total_reflections_;

					auto reflected = request_and_fold( model::effort::high );

					if ( !reflected ) {
						finish_run( loop_state::handoff, reflected.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					if ( *reflected ) {
						++last_failure_repeats_;

						if ( last_failure_repeats_ >= REFLECT_REPEAT_LIMIT ) {
							// these calls still run: a tool_call with no result is rejected.
							dispatch_pending( );
							state_ = loop_state::observe;

							break;
						}
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::replan: {
					if ( replan_count_ >= REPLAN_GUARD_LIMIT ) {
						finish_run( loop_state::handoff, "no new plan" );
						state_ = loop_state::handoff;

						break;
					}

					++replan_count_;

					auto replanned = request_and_fold( model::effort::high );

					if ( !replanned ) {
						finish_run( loop_state::handoff, replanned.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::handoff:
				case loop_state::done: {
					// The log needs a run.end even when the turn completed without a prior
					// finish_run, but `summary_json` is the run-level "gave up" signal the
					// exit-code mapping reads: setting it here would turn a completed run into
					// a provider error. The event is appended directly, and the outcome keeps
					// the empty summary that means "completed".
					if ( end_reason_.empty( ) ) {
						auto payload = std::string{ "{\"reason\":\"" };
						json::append_escaped( payload, state_ == loop_state::done
								? "turn complete" : "handed off" );
						payload += "\",\"state\":\"";
						payload += to_string( state_ );
						payload += "\"}";

						log_->append( "run.end", std::move( payload ) );
					}

					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.steps = budget_.steps_used.load( );
					outcome.summary_json = end_reason_;

					return outcome;
				}

				case loop_state::failed: {
					finish_run( loop_state::failed, "budget exhausted during plan" );
					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.summary_json = "budget exhausted during plan";

					return outcome;
				}

				case loop_state::idle: {
					state_ = loop_state::plan;

					break;
				}
			}

			publish( events::kind::step_end, std::string{ "{\"state\":\"" }
				+ std::string{ to_string( state_ ) } + "\"}" );

			if ( auto compacted = maybe_compact( ); !compacted ) {
				finish_run( loop_state::handoff, compacted.error( ).msg );
				state_ = loop_state::handoff;
			}
		}

	}

} // namespace mcode
