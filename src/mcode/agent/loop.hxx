#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/agent/repetition_guard.hxx"
#include "mcode/agent/context_controller.hxx"
#include "mcode/core/error.hxx"
#include "mcode/agent/event_log.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/model/client.hxx"
#include "mcode/model/types.hxx"
#include "mcode/support/text.hxx"

namespace mcode::perm {
	class permission_engine;
}

namespace mcode {

	class snapshot_store;

	// Declared, not included: `subagent.hxx` includes this header, so the spawn is
	// defined alongside the subagent it builds.
	struct subagent_spec;
	struct subagent_report;

	// Which call this is, not how big it is: routing is by task class.
	enum class model_tier {
		plan,
		act,
		side,
	};

	// A tier's model and its prices. Unconfigured means "use the loop's own model",
	// which is the fallback `model.model` names.
	struct tier_model {
		std::string name;
		model::capabilities caps = { };
		bool configured = false;
	};

	inline constexpr std::uint32_t DEFAULT_MAX_STEPS = 100;
	inline constexpr std::uint64_t DEFAULT_MAX_TOKENS = 2'000'000;
	inline constexpr double DEFAULT_MAX_USD = 5.0;
	inline constexpr double NEARLY_EXHAUSTED_FRACTION = 0.20;

	// The goal is rewritten into the prompt this often, and after every compaction:
	// multi-step plans drift by roughly 70% fill, and recitation is the cheap fix.
	inline constexpr std::uint32_t RECITATION_INTERVAL_STEPS = 10;

	inline constexpr std::size_t THRASH_WINDOW = 12;
	inline constexpr std::size_t THRASH_REPEAT_LIMIT = 3;

	inline constexpr std::size_t DOOM_LOOP_THRESHOLD = 3;

	inline constexpr std::size_t MAX_REFLECTIONS_PER_FAILURE_CLASS = 2;
	inline constexpr std::size_t MAX_REFLECTIONS_PER_RUN = 4;

	inline constexpr std::size_t THRASH_ESCALATION_FACTOR = 2;
	inline constexpr std::size_t REPLAN_GUARD_LIMIT = 2;
	inline constexpr std::size_t REFLECT_REPEAT_LIMIT = 2;

	// Retries saturate by the third attempt; a fourth only spends budget.
	inline constexpr std::size_t MAX_TOOL_CALL_RETRIES = 3;

	inline constexpr double COMPACTION_TRIGGER_FRACTION = 0.80;

	// Without a floor, a declared window at or below RESERVED_OUTPUT_TOKENS never
	// triggers compaction.
	inline constexpr double MIN_USABLE_WINDOW_FRACTION = 0.10;

	inline constexpr double SAFETY_MARGIN_FRACTION = 0.05;
	inline constexpr std::int64_t RESERVED_OUTPUT_TOKENS = 16'000;

	inline constexpr std::int64_t SYSTEM_PROMPT_TOKEN_BUDGET = 1'500;
	inline constexpr std::int64_t TOOLS_TOKEN_BUDGET = 3'500;
	inline constexpr std::int64_t INSTRUCTION_CHAIN_TOKEN_BUDGET = 2'000;
	inline constexpr std::int64_t SKILL_INDEX_TOKEN_BUDGET = 1'500;
	inline constexpr std::int64_t SESSION_START_TOKEN_BUDGET = 8'500;

	inline constexpr std::size_t CHARS_PER_TOKEN_ESTIMATE = mcode::text::CHARS_PER_TOKEN;

	inline constexpr std::size_t COMPACTION_KEEP_FIRST_EVENTS = 4;
	inline constexpr std::size_t COMPACTION_KEEP_LAST_TURNS = 3;
	inline constexpr std::int64_t COMPACTION_KEEP_LAST_TOKENS = 20'000;

	// Charged on the turn thread and read by the REPL pump, so the counters are atomic.
	struct session_budget {
		std::uint32_t max_steps = DEFAULT_MAX_STEPS;
		std::uint64_t max_tokens = DEFAULT_MAX_TOKENS;
		double max_usd = DEFAULT_MAX_USD;

		std::atomic< std::uint32_t > steps_used{ 0 };
		std::atomic< std::uint64_t > tokens_used{ 0 };
		std::atomic< double > usd_used{ 0.0 };

		session_budget( ) = default;

		// atomics do not copy, and the budget is handed to the loop by value.
		session_budget( const session_budget& other )
			: max_steps( other.max_steps ), max_tokens( other.max_tokens ),
			max_usd( other.max_usd ),
			steps_used( other.steps_used.load( ) ), tokens_used( other.tokens_used.load( ) ),
			usd_used( other.usd_used.load( ) ) { }

		auto operator=( const session_budget& other ) -> session_budget& {
			max_steps = other.max_steps;
			max_tokens = other.max_tokens;
			max_usd = other.max_usd;
			steps_used.store( other.steps_used.load( ) );
			tokens_used.store( other.tokens_used.load( ) );
			usd_used.store( other.usd_used.load( ) );

			return *this;
		}

		[[nodiscard]] auto exhausted( ) const noexcept -> bool {
			return steps_used.load( ) >= max_steps || tokens_used.load( ) >= max_tokens ||
				usd_used.load( ) >= max_usd;
		}

		// `exhausted()` is a disjunction; naming the wrong limit contradicts the
		// remaining-budget fields in the same summary.
		[[nodiscard]] auto exhaustion_reason( ) const -> std::string {
			if ( steps_used.load( ) >= max_steps ) {
				return "step budget exhausted (" + std::to_string( steps_used.load( ) ) +
					" of " + std::to_string( max_steps ) + " steps)";
			}

			if ( tokens_used.load( ) >= max_tokens ) {
				return "token budget exhausted (" + std::to_string( tokens_used.load( ) ) +
					" of " + std::to_string( max_tokens ) + " tokens)";
			}

			return "cost budget exhausted (" + std::to_string( usd_used.load( ) ) + " of " +
				std::to_string( max_usd ) + " usd)";
		}

		[[nodiscard]] auto nearly_exhausted( ) const noexcept -> bool {
			if ( max_usd <= 0.0 ) {
				return exhausted( );
			}

			return ( 1.0 - usd_used.load( ) / max_usd ) < NEARLY_EXHAUSTED_FRACTION;
		}

		auto charge( const std::uint64_t tokens, const double usd ) noexcept -> void {
			steps_used.fetch_add( 1 );
			tokens_used.fetch_add( tokens );

			// atomic<double> has no fetch_add in the standard, so the add is a compare-exchange loop.
			auto current = usd_used.load( );

			while ( !usd_used.compare_exchange_weak( current, current + usd ) ) {
			}
		}

		// Folds a subagent's spend into this budget. A child on its own budget would be
		// free to the run, so the parent carries what the child cost. The child's step
		// count is its own; only its tokens and cost are the run's.
		auto absorb_child_spend( const std::uint64_t tokens, const double usd ) noexcept -> void {
			tokens_used.fetch_add( tokens );

			auto current = usd_used.load( );

			while ( !usd_used.compare_exchange_weak( current, current + usd ) ) {
			}
		}
	};

	struct tool_call {
		std::string id;
		std::string name;
		std::string args_json;

		// The response hit the output limit, so unparseable arguments read as truncated.
		bool truncated = false;
	};

	struct tool_outcome {
		bool ok = false;
		std::string content;
		errc code = errc::ok;
		std::string error_message;
		std::chrono::milliseconds elapsed{ 0 };

		// Stable across attempts, unlike `error_message`, which may embed a changing value.
		std::string failure_class;

		// One denial is a failed call, not the end of the run; cleared by any later success.
		bool permission_denied = false;
	};

	enum class loop_state {
		idle,
		plan,
		act,
		observe,
		verify,
		reflect,
		replan,
		handoff,
		done,
		failed,
	};

	[[nodiscard]] auto to_string( const loop_state value ) noexcept -> std::string_view;

	// `no_tests` is not `verified`: a run with no verification command never
	// established success.
	enum class verification_verdict {
		no_tests,
		verified,
		failed,
	};

	[[nodiscard]] auto to_string( verification_verdict value ) noexcept -> std::string_view;

	struct turn_outcome {
		loop_state final_state = loop_state::idle;

		verification_verdict verdict = verification_verdict::no_tests;

		std::vector< loop_state > visited;

		// Empty when the turn completed rather than gave up.
		std::string summary_json;

		std::size_t steps = 0;
	};

	struct assembled_request {
		model::chat_request request;
	};

	struct assemble_request_options {
		std::string_view system_prompt;
		const std::vector< model::message >& history;
		std::string_view model_name;
		model::cache_mode mode = model::cache_mode::none;
		bool near_budget = false;

		// Regenerated from session state, not summarized: it sits ahead of the history
		// so compaction cannot drop the goal, and it is byte-identical between
		// rewrites so the prefix stays cached.
		std::string_view pinned_state;

		// The provider's field names, so a breakpoint anchor matches the rendered body.
		model::request_spec fields = { };

		bool strict_tools = false;

		bool response_format_with_tools = false;
	};

	// The prefix order (tools, system, messages) is byte-stable; mutable state lives in the tail.
	[[nodiscard]] auto assemble_request( const tool_registry& registry,
		const assemble_request_options& options ) -> assembled_request;

	// Sections 1-8 are cache-stable; the environment block lives in the tail.
	[[nodiscard]] auto build_system_prompt( const tool_registry& registry,
		const std::string_view instruction_chain = { },
		const std::string_view skill_index = { },
		const std::string_view notes_pointer = { } ) -> std::string;

	class thrash_detector {
	public:
		auto record( const std::string_view tool_name, const std::string_view args_json )
			-> std::size_t;

		[[nodiscard]] auto repeat_count( ) const noexcept -> std::size_t {
			return current_repeats_;
		}

	private:
		std::vector< std::string > window_;
		std::size_t current_repeats_ = 0;
	};

	class agent_loop {
	public:
		using tool_handler = std::function< result< std::string >( std::string_view args_json ) >;

		// The loop owns nothing it is handed: the registry, client, log and bus outlive it.
		struct dependencies {
			tool_registry* registry = nullptr;
			model::model_client* client = nullptr;
			event_log* log = nullptr;
			events::bus* bus = nullptr;
			session_budget budget = { };
			std::string model_name;
			model::capabilities caps = { };

			// Routing is by call site, and an unset tier falls back to `model_name`.
			// Resolved by the caller, which is the only place that has the config.
			tier_model plan_tier;
			tier_model act_tier;
			tier_model side_tier;

			model::provider_descriptor provider;
			std::string api_key;
			std::string workspace_root;
			std::string platform_name;

			// Non-const: approval resolution mutates it (session rules, store writes).
			perm::permission_engine* permissions = nullptr;

			std::string instruction_chain;
			std::string skill_index;

			// When set, write-class targets are captured for /undo and /rewind.
			snapshot_store* snapshots = nullptr;

			// Runs on the loop's thread once per step, which is when extension timers fire.
			std::function< void( ) > pump_timers;

			// Same step, after the timers: a timer handler that defers must not run its
			// callback inside the timer that queued it.
			std::function< void( ) > pump_deferred;
		};

		agent_loop( tool_registry& registry, event_log& log, session_budget budget = { } )
			: registry_( &registry ), log_( &log ), budget_( budget ) { }

		// Named `deps`, not `dependencies`: GCC rejects a parameter shadowing a member (-Wshadow).
		explicit agent_loop( dependencies deps );

		[[nodiscard]] auto execute( const tool_call& call ) -> tool_outcome;

		auto register_handler( const std::string name, tool_handler handler ) -> void;

		[[nodiscard]] auto handler_for( const std::string_view name ) const
			-> const tool_handler* {
			const auto found = handlers_.find( std::string{ name } );

			return found != handlers_.end( ) ? &found->second : nullptr;
		}

		// The child borrows this loop's collaborators, its remaining budget and its
		// workspace; it does not borrow the conversation. Defined in `subagent.cxx`.
		[[nodiscard]] auto spawn_subagent( const subagent_spec& spec )
			-> result< subagent_report >;

		// The model a tier resolves to, falling back to this loop's own model.
		[[nodiscard]] auto tier_for( model_tier tier ) const -> const tier_model&;

		// The model the loop does its main work on: what the window and the context
		// meter are measured against.
		[[nodiscard]] auto working_model( ) const noexcept -> const model::capabilities&;

		// a plan response with no tool call is the turn's answer, so Act never re-requests it.
		[[nodiscard]] auto run( const std::string_view user_task ) -> result< turn_outcome >;

		// Owned by the caller, set from another thread; read at each step boundary.
		auto set_cancel_source( const std::atomic< bool >* value ) -> void {
			cancel_ = value;
		}

		[[nodiscard]] auto budget( ) const noexcept -> const session_budget& { return budget_; }

		[[nodiscard]] auto bus( ) const noexcept -> events::bus& { return *bus_; }
		[[nodiscard]] auto budget( ) noexcept -> session_budget& { return budget_; }
		[[nodiscard]] auto log( ) const noexcept -> const event_log& { return *log_; }

		[[nodiscard]] auto registry( ) const noexcept -> const tool_registry& {
			return *registry_;
		}

		auto set_verification_command( const std::string command ) -> void {
			verification_command_ = std::move( command );
		}

		auto set_notes_pointer( std::string pointer ) -> void {
			notes_pointer_ = std::move( pointer );
		}

		// The plan lives in session state, so recitation reads it whatever wrote it —
		// the `todo` extension when loaded, and nothing at all when it is disabled.
		// Refreshed here rather than waiting for the interval: a plan set mid-run is
		// wanted on the very next request, which is the turn that acts on it.
		auto set_plan( std::string plan ) -> void {
			plan_ = std::move( plan );
			refresh_pinned_state( );
		}

		[[nodiscard]] auto plan( ) const noexcept -> std::string_view { return plan_; }

		[[nodiscard]] auto state( ) const noexcept -> loop_state { return state_; }

		[[nodiscard]] auto model_name( ) const noexcept -> std::string_view {
			return model_name_;
		}

		// Window occupancy, not cumulative spend, which is `tokens_spent()`.
		[[nodiscard]] auto context_used( ) const noexcept -> std::uint64_t {
			const auto prompt = occupancy_.prompt_tokens;

			return prompt > 0 ? static_cast< std::uint64_t >( prompt ) : 0;
		}

		[[nodiscard]] auto tokens_spent( ) const noexcept -> std::uint64_t {
			return budget_.tokens_used.load( );
		}

		[[nodiscard]] auto occupancy( ) const noexcept -> const context_occupancy& {
			return occupancy_;
		}

		[[nodiscard]] auto verification_detail( ) const noexcept -> std::string_view {
			return verification_detail_;
		}

		[[nodiscard]] auto context_capacity( ) const noexcept -> std::uint64_t {
			const auto& working = working_model( );

			return working.context_window > 0
				? static_cast< std::uint64_t >( working.context_window )
				: 0;
		}

		[[nodiscard]] auto history( ) const noexcept -> const std::vector< model::message >& {
			return history_;
		}

		auto seed_history( std::vector< model::message > restored ) -> void;

		// `seed_history` alone leaves `run_id()`, the budget counters and `state()`
		// describing the session just left; the budget limits are kept.
		auto reset_session( std::vector< model::message > restored ) -> void;

		[[nodiscard]] auto approval_mode( ) const noexcept -> std::string_view;

		[[nodiscard]] auto instruction_chain( ) const noexcept -> std::string_view {
			return instruction_chain_;
		}

		[[nodiscard]] auto workspace_root( ) const noexcept -> std::string_view {
			return workspace_root_;
		}

		[[nodiscard]] auto snapshots( ) const noexcept -> snapshot_store* { return snapshots_; }

		[[nodiscard]] auto run_id( ) const noexcept -> std::string_view {
			return snapshot_run_id_;
		}

		// The run-level exit-5 signal: a denial that leaves the loop unable to progress.
		[[nodiscard]] auto permission_denied( ) const noexcept -> bool {
			return permission_denied_;
		}

		auto observe_result( const tool_call& call, const tool_outcome& outcome ) -> void;

		// Built in one place so a completed run and one that gave up carry the same fields.
		[[nodiscard]] auto run_end_payload( std::string_view reason ) const -> std::string;

		auto finish_run( const loop_state terminal, const std::string_view reason ) -> void;

	private:
		auto run_state_machine( ) -> turn_outcome;

		auto request_and_fold( model::effort effort, model_tier tier ) -> result< bool >;

		// One side-tier call that compresses the messages about to be dropped, so the
		// history keeps a record of them. Empty on failure: the caller then drops them
		// without a summary, which is the documented fallback, never a lost goal.
		[[nodiscard]] auto summarize_dropped( const std::vector< model::message >& dropped )
			-> std::string;

		auto dispatch_pending( ) -> void;

		auto resample_pending_calls( ) -> bool;

		auto dispatch_calls( const std::vector< tool_call >& calls ) -> bool;

		[[nodiscard]] auto refuse_doom_loop( const tool_call& call, const std::size_t repeats )
			-> tool_outcome;

		// A capture failure is logged and swallowed: an unusable store must not block
		// a working edit.
		auto capture_before_write( const tool_call& call, const tool_class klass ) -> void;

		auto mint_run_id( ) -> void;

		auto maybe_compact( ) -> status;
		auto publish( const events::kind type, std::string payload_json ) -> void;

		tool_registry* registry_ = nullptr;
		model::model_client* client_ = nullptr;
		event_log* log_ = nullptr;
		events::bus* bus_ = nullptr;

		tool_registry owned_registry_;
		event_log owned_log_;
		events::bus owned_bus_;

		session_budget budget_;
		loop_state state_ = loop_state::idle;

		std::vector< model::message > history_;
		std::vector< loop_state > visited_;
		std::string user_task_;
		std::string model_name_;
		model::capabilities caps_;

		// The fallback `tier_for` returns: this loop's own model and prices.
		tier_model default_tier_;
		tier_model plan_tier_;
		tier_model act_tier_;
		tier_model side_tier_;
		model::provider_descriptor provider_;
		std::string api_key_;
		std::string workspace_root_;
		std::string platform_name_;
		snapshot_store* snapshots_ = nullptr;
		std::string snapshot_run_id_;

		std::uint32_t turn_index_ = 0;

		thrash_detector thrash_;
		std::string last_failure_;
		std::string last_failure_class_;
		std::string end_reason_;

		std::vector< std::string > previous_texts_;
		std::size_t repetition_steers_ = 0;

		std::string last_response_text_;

		// Returns false when the steer budget is spent, so the caller falls through.
		[[nodiscard]] auto steer_repetition( const agent::repetition_verdict& verdict ) -> bool;

		// Rebuilds the block from session state. Cheap, so it runs on the recitation
		// interval and after a compaction rather than every request.
		auto refresh_pinned_state( ) -> void;

		std::string stop_reason_;
		bool turn_truncated_ = false;

		std::size_t tool_call_retries_ = 0;

		model::usage run_usage_;

		bool hard_error_ = false;
		bool permission_denied_ = false;
		std::string verification_command_;
		std::string verification_detail_;
		verification_verdict verdict_ = verification_verdict::no_tests;
		bool harness_call_ = false;
		std::string notes_pointer_;
		context_occupancy occupancy_;

		// Recitation: the goal, the plan and the files this run changed, rebuilt from
		// session state so a compaction cannot lose them.
		pinned_state pinned_;
		std::string task_;
		std::string plan_;
		std::vector< std::string > modified_files_;
		std::uint32_t last_recitation_step_ = 0;
		perm::permission_engine* permissions_ = nullptr;
		std::string instruction_chain_;
		std::string skill_index_;
		std::function< void( ) > pump_timers_;
		std::function< void( ) > pump_deferred_;
		const std::atomic< bool >* cancel_ = nullptr;
		std::vector< tool_call > pending_calls_;

		bool plan_answered_ = false;
		std::map< std::string, std::size_t, std::less<> > reflection_counts_;
		std::size_t total_reflections_ = 0;
		std::size_t replan_count_ = 0;
		std::size_t last_failure_repeats_ = 0;

		std::map< std::string, tool_handler, std::less<> > handlers_;
	};

} // namespace mcode
