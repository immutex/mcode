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

	auto agent_loop::run_end_payload( const std::string_view reason ) const -> std::string {
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

		// No leading quote: the previous field is a number, so a `"` here closes
		// a string that was never open and makes the whole summary unparseable.
		// Every `run.end` record was invalid JSON because of it.
		summary += ",\"remaining_usd\":";
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
		summary += to_string( state_ );
		summary += "\",\"reason\":\"";
		json::append_escaped( summary, reason );

		// Closes the `reason` string before the object. Without the quote the
		// reason runs into the closing brace and the summary does not parse.
		summary += "\"}";

		return summary;
	}

	auto agent_loop::finish_run( const loop_state terminal, const std::string_view reason )
		-> void {
		state_ = terminal;
		end_reason_ = std::string{ reason };

		auto summary = run_end_payload( reason );

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

	auto agent_loop::reset_session( std::vector< model::message > restored ) -> void {
		history_ = std::move( restored );

		// Zero the counters, not the limits: `max_steps` and friends come from the
		// command line and describe this process.
		budget_.steps_used.store( 0 );
		budget_.tokens_used.store( 0 );
		budget_.usd_used.store( 0.0 );

		// A fresh capture group, so `/undo` before the next turn finds nothing
		// rather than reverting the previous session's last run.
		mint_run_id( );

		state_ = loop_state::idle;
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

		++turn_index_;

		if ( log_ != nullptr ) {
			log_->set_position( snapshot_run_id_, turn_index_, 0 );
		}

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

		// `turn_end` must fire on EVERY exit, including the early returns below, and
		// the step count must be reported on every one of them. It was set only on
		// the done/handoff branch, so an interrupted or provider-failed run told the
		// caller it had taken zero steps.
		struct turn_end_guard {
			agent_loop* self;
			turn_outcome* outcome;

			~turn_end_guard( ) {
				outcome->steps = self->budget_.steps_used.load( );
				self->publish( events::kind::turn_end, "{}" );
			}
		} const guard{ this, &outcome };

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

			// Every event from here until the next boundary is this step's.
			if ( log_ != nullptr ) {
				log_->set_position( snapshot_run_id_, turn_index_,
					budget_.steps_used.load( ) + 1 );
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
						const auto reason = budget_.exhaustion_reason( );

						finish_run( loop_state::handoff, reason );
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
						const auto reason = budget_.exhaustion_reason( );

						finish_run( loop_state::handoff, reason );
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
						// The same builder the gave-up path uses, so a completed run
						// carries the token totals, the cost and the step count too.
						// It used to append only `reason` and `state`, which left a
						// successful run's log without any of them.
						log_->append( "run.end", run_end_payload( state_ == loop_state::done
								? "turn complete" : "handed off" ) );
					}

					outcome.final_state = state_;
					outcome.visited = visited_;
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
