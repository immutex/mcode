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

	auto to_string( const verification_verdict value ) noexcept -> std::string_view {
		switch ( value ) {
			case verification_verdict::no_tests: return "no-tests";
			case verification_verdict::verified: return "verified";
			case verification_verdict::failed: return "failed";
		}

		return "no-tests";
	}

	agent_loop::agent_loop( dependencies deps )
		: registry_( deps.registry ), client_( deps.client ),
		log_( deps.log ), bus_( deps.bus ), budget_( deps.budget ),
		model_name_( deps.model_name ), caps_( deps.caps ),
		plan_tier_( std::move( deps.plan_tier ) ), act_tier_( std::move( deps.act_tier ) ),
		side_tier_( std::move( deps.side_tier ) ),
		provider_( deps.provider ), api_key_( deps.api_key ),
		workspace_root_( deps.workspace_root ),
		platform_name_( deps.platform_name ),
		snapshots_( deps.snapshots ),
		permissions_( deps.permissions ),
		instruction_chain_( deps.instruction_chain ),
		skill_index_( deps.skill_index ),
		pump_timers_( std::move( deps.pump_timers ) ),
		pump_deferred_( std::move( deps.pump_deferred ) ) {
		default_tier_ = tier_model{ .name = model_name_, .caps = caps_, .configured = true };

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

	auto agent_loop::run( const std::string_view user_task ) -> result< turn_outcome > {
		user_task_ = std::string{ user_task };
		task_ = std::string{ user_task };
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

		// A fresh run restates the goal; the plan survives, because it is session state.
		modified_files_.clear( );
		refresh_pinned_state( );

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

		// Recorded so `--continue` restores the conversation.
		if ( log_ != nullptr && !history_.empty( ) ) {
			log_->append( std::string{ agent::MESSAGE_USER_EVENT }, agent::message_to_json( history_.back( ) ) );
		}

		return run_state_machine( );
	}

	auto agent_loop::run_state_machine( ) -> turn_outcome {
		auto outcome = turn_outcome{ };

		state_ = loop_state::plan;

		publish( events::kind::turn_start, "{}" );

		// `turn_end` and the step count must fire on every exit, early returns included.
		struct turn_end_guard {
			agent_loop* self;
			turn_outcome* outcome;

			~turn_end_guard( ) {
				outcome->steps = self->budget_.steps_used.load( );
				self->publish( events::kind::turn_end, "{}" );
			}
		} const guard{ this, &outcome };

		while ( true ) {
			if ( cancel_ != nullptr && cancel_->load( ) ) {
				finish_run( loop_state::handoff, "interrupted" );
				outcome.final_state = loop_state::handoff;
				outcome.visited = visited_;
				outcome.verdict = verdict_;
				outcome.summary_json = "interrupted";

				return outcome;
			}

			if ( log_ != nullptr ) {
				log_->set_position( snapshot_run_id_, turn_index_,
					budget_.steps_used.load( ) + 1 );
			}

			publish( events::kind::step_start, std::string{ "{\"state\":\"" }
				+ std::string{ to_string( state_ ) } + "\"}" );

			if ( pump_timers_ ) {
				pump_timers_( );
			}

			if ( pump_deferred_ ) {
				pump_deferred_( );
			}

			// Every N steps: cheap, and it is what keeps a long plan from drifting.
			if ( budget_.steps_used.load( ) >= last_recitation_step_ + RECITATION_INTERVAL_STEPS ) {
				refresh_pinned_state( );
			}

			visited_.push_back( state_ );

			switch ( state_ ) {
				case loop_state::plan: {
					if ( budget_.steps_used.load( ) >= budget_.max_steps ) {
						state_ = loop_state::failed;

						break;
					}

					auto planned = request_and_fold( model::effort::high, model_tier::plan );

					if ( !planned ) {
						finish_run( loop_state::failed, planned.error( ).msg );
						outcome.final_state = state_;
						outcome.visited = visited_;
						outcome.verdict = verdict_;
						outcome.summary_json = planned.error( ).msg;

						return outcome;
					}

					plan_answered_ = !*planned;
					state_ = loop_state::act;

					break;
				}

				case loop_state::act: {
					// Dispatched ahead of the budget check, so a spent step still answers its calls.
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

					auto acted = request_and_fold( model::effort::medium, model_tier::act );

					if ( !acted ) {
						finish_run( loop_state::handoff, acted.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

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

					// Consulted ahead of the failure branch: the repeat is the informative reading.
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
						verdict_ = verification_verdict::no_tests;
						state_ = loop_state::handoff;

						break;
					}

					auto arguments = std::string{ "{\"command\":\"" };
					json::append_escaped( arguments, verification_command_ );
					arguments += "\"}";

					// Harness-owned, so the agent's approval policy does not apply to it.
					harness_call_ = true;
					auto checked = execute( tool_call{ std::string{ }, "bash", arguments } );
					harness_call_ = false;

					// A non-zero exit is still a successful tool call; the gate reads the code.
					auto passed = checked.ok;

					if ( passed ) {
						auto parsed = json::document::parse( checked.content );

						if ( parsed ) {
							const auto code = parsed->pointer_int( "/exit_code" );
							const auto timed_out = parsed->pointer_bool( "/timed_out" );

							passed = code.has_value( ) && *code == 0 &&
								( !timed_out.has_value( ) || !*timed_out );

							// The exit code separates a real failure from a broken check.
							if ( !passed ) {
								verification_detail_ = code.has_value( )
									? "the command exited " + std::to_string( *code )
									: "the command reported no exit code";
							}
						} else {
							passed = false;
							verification_detail_ = "the command's result did not parse";
						}
					} else {
						verification_detail_ = checked.error_message;
					}

					if ( passed ) {
						verdict_ = verification_verdict::verified;
						verification_detail_.clear( );
						state_ = loop_state::done;

						break;
					}

					verdict_ = verification_verdict::failed;
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

					auto reflected = request_and_fold( model::effort::high, model_tier::plan );

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

					auto replanned = request_and_fold( model::effort::high, model_tier::plan );

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
					// `summary_json` is the run-level "gave up" signal the exit-code mapping
					// reads, so the completed path leaves it empty.
					if ( end_reason_.empty( ) ) {
						log_->append( "run.end", run_end_payload( state_ == loop_state::done
								? "turn complete" : "handed off" ) );
					}

					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.verdict = verdict_;
					outcome.summary_json = end_reason_;

					return outcome;
				}

				case loop_state::failed: {
					finish_run( loop_state::failed, "budget exhausted during plan" );
					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.verdict = verdict_;
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
