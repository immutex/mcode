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
#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/model/client.hxx"
#include "mcode/model/types.hxx"

namespace mcode::perm {
	class permission_engine;
}

namespace mcode {

	class snapshot_store;

	inline constexpr std::uint32_t DEFAULT_MAX_STEPS = 100;
	inline constexpr std::uint64_t DEFAULT_MAX_TOKENS = 2'000'000;
	inline constexpr double DEFAULT_MAX_USD = 5.0;
	inline constexpr double NEARLY_EXHAUSTED_FRACTION = 0.20;

	inline constexpr std::size_t THRASH_WINDOW = 12;
	inline constexpr std::size_t THRASH_REPEAT_LIMIT = 3;

	// Identical dispatches after which the call is refused, not merely escalated as thrash.
	inline constexpr std::size_t DOOM_LOOP_THRESHOLD = 3;

	inline constexpr std::size_t MAX_REFLECTIONS_PER_FAILURE_CLASS = 2;
	inline constexpr std::size_t MAX_REFLECTIONS_PER_RUN = 4;

	inline constexpr std::size_t THRASH_ESCALATION_FACTOR = 2;
	inline constexpr std::size_t REPLAN_GUARD_LIMIT = 2;
	inline constexpr std::size_t REFLECT_REPEAT_LIMIT = 2;

	// Resampling a response whose tool arguments will not parse. Retries saturate by the third
	// attempt, so a fourth only spends budget on a call the model cannot produce.
	inline constexpr std::size_t MAX_TOOL_CALL_RETRIES = 3;

	inline constexpr double COMPACTION_TRIGGER_FRACTION = 0.80;
	inline constexpr double TOOL_CLEAR_TRIGGER_FRACTION = 0.60;
	inline constexpr double CLEAR_AT_LEAST_FRACTION = 0.20;
	inline constexpr double SAFETY_MARGIN_FRACTION = 0.05;
	inline constexpr std::int64_t RESERVED_OUTPUT_TOKENS = 16'000;

	inline constexpr std::size_t MAX_POST_COMPACT_REREADS = 5;
	inline constexpr std::int64_t POST_COMPACT_REREAD_TOKENS = 5'000;

	inline constexpr std::int64_t SYSTEM_PROMPT_TOKEN_BUDGET = 1'500;
	inline constexpr std::int64_t TOOLS_TOKEN_BUDGET = 3'500;
	inline constexpr std::int64_t INSTRUCTION_CHAIN_TOKEN_BUDGET = 2'000;
	inline constexpr std::int64_t SKILL_INDEX_TOKEN_BUDGET = 1'500;
	inline constexpr std::int64_t SESSION_START_TOKEN_BUDGET = 8'500;

	inline constexpr std::size_t CHARS_PER_TOKEN_ESTIMATE = 4;

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

		// Which limit actually bound. `exhausted()` is a disjunction, and reporting
		// all three as "budget exhausted" produced a record that contradicted
		// itself: a run that spent its 60 steps printed "budget exhausted" next to
		// `remaining_usd: 0.567925`. A reader concludes the cost accounting is
		// broken, and a run stopped for steps is a different problem from one
		// stopped for money.
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
	};

	struct event {
		std::uint32_t version = 1;
		std::uint64_t sequence = 0;
		std::int64_t timestamp_ms = 0;
		std::string kind;
		std::string run;
		std::uint32_t turn = 0;
		std::uint32_t step = 0;
		std::string payload_json;

		[[nodiscard]] auto to_json( ) const -> std::string;
	};

	// Flushed per event so a crash keeps every complete line; replay tolerates a torn tail.
	class event_log {
	public:
		event_log( ) = default;
		~event_log( );

		event_log( event_log&& other ) noexcept;
		auto operator=( event_log&& other ) noexcept -> event_log&;

		event_log( const event_log& ) = delete;
		auto operator=( const event_log& ) -> event_log& = delete;

		auto open( const std::filesystem::path& path ) -> status;
		auto close( ) -> void;

		[[nodiscard]] auto is_open( ) const noexcept -> bool { return sink_ != nullptr; }
		[[nodiscard]] auto path( ) const noexcept -> const std::filesystem::path& { return path_; }

		// A failed flush is counted, never thrown: losing the log must not end the session.
		[[nodiscard]] auto write_failures( ) const noexcept -> std::uint64_t {
			return write_failures_;
		}

		auto append( const std::string kind, std::string payload_json = "{}" ) -> event;

		// Not append(): this preserves the recorded sequence and must not renumber.
		auto restore( event recorded ) -> void;

		[[nodiscard]] auto events( ) const noexcept -> const std::vector< event >& {
			return events_;
		}
		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return events_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return events_.empty( ); }


		[[nodiscard]] auto next_sequence( ) const noexcept -> std::uint64_t {
			return next_sequence_;
		}


		[[nodiscard]] auto to_jsonl( ) const -> std::string;

	private:
		std::vector< event > events_;
		std::uint64_t next_sequence_ = 0;

		std::unique_ptr< std::FILE, void ( * )( std::FILE* ) > sink_{ nullptr, nullptr };
		std::filesystem::path path_;
		std::uint64_t write_failures_ = 0;
	};

	struct replay_result {
		event_log log;

		std::size_t events_read = 0;

		bool truncated_tail = false;

		std::size_t malformed_lines = 0;
	};

	[[nodiscard]] auto replay_event_log( const std::filesystem::path& path )
		-> result< replay_result >;

	struct tool_call {
		std::string id;
		std::string name;
		std::string args_json;

		// Set when the response that carried this call hit the output limit, so a call whose
		// arguments will not parse is reported as truncated rather than as malformed.
		bool truncated = false;
	};

	struct tool_outcome {
		bool ok = false;
		std::string content;
		errc code = errc::ok;
		std::string error_message;
		std::chrono::milliseconds elapsed{ 0 };

		// A stable name for what went wrong, used by the loop to decide whether it has
		// already reflected on this kind of failure. Empty means fall back to the message.
		// Distinct from `error_message`, which is written for the model and may embed a
		// value that changes on every attempt.
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

	struct turn_outcome {
		loop_state final_state = loop_state::idle;

		std::vector< loop_state > visited;

		// the reason finish_run recorded; empty when the turn completed rather than gave up.
		std::string summary_json;

		// session_budget::steps_used at the end of the turn, the same count the CLI reports.
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
		std::string_view recitation;

		// the provider's field names, so a breakpoint anchor matches the rendered body.
		model::request_spec fields = { };

		// Constrain tool-call generation to the schema where the provider supports it.
		bool strict_tools = false;

		// Whether a JSON response format may accompany a tool list.
		bool response_format_with_tools = false;
	};

	// The prefix order (tools, system, messages) is byte-stable; mutable state lives in the tail.
	[[nodiscard]] auto assemble_request( const tool_registry& registry,
		const assemble_request_options& options ) -> assembled_request;

	// Sections 1-8 are cache-stable; the environment block and recitation live in the tail.
	[[nodiscard]] auto build_system_prompt( const tool_registry& registry,
		const std::string_view instruction_chain = { },
		const std::string_view skill_index = { } ) -> std::string;

	class thrash_detector {
	public:
		// Returns the repeat count of this exact hash within the window.
		auto record( const std::string_view tool_name, const std::string_view args_json )
			-> std::size_t;

		[[nodiscard]] auto repeat_count( ) const noexcept -> std::size_t {
			return current_repeats_;
		}

	private:
		std::vector< std::string > window_;
		std::size_t current_repeats_ = 0;
	};

	struct compaction_result {
		std::string summary;
		std::vector< std::string > pinned_facts;
		std::vector< std::string > decisions;
		std::vector< std::string > open_questions;

		std::vector< model::message > kept;
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
			std::string provider_name;
			model::provider_descriptor provider;
			std::string api_key;
			std::string workspace_root;
			std::string platform_name;

			// Non-const: approval resolution mutates it (session rules, store writes).
			perm::permission_engine* permissions = nullptr;

			std::string instruction_chain;
			std::string skill_index;

			// Optional: when set, the bytes of every write-class target are captured before
			// the handler runs, which is what /undo and /rewind restore from.
			snapshot_store* snapshots = nullptr;

			// called once per step, on the loop's thread; extension timers fire from here.
			std::function< void( ) > pump_timers;
		};

		agent_loop( tool_registry& registry, event_log& log, session_budget budget = { } )
			: registry_( &registry ), log_( &log ), budget_( budget ) { }

		// Named `deps`, not `dependencies`: GCC rejects a parameter shadowing a member (-Wshadow).
		explicit agent_loop( dependencies deps );

		[[nodiscard]] auto execute( const tool_call& call ) -> tool_outcome;

		auto register_handler( const std::string name, tool_handler handler ) -> void;

		// Exposes the dispatch entry the loop itself calls, for tests and the MCP connect path.
		[[nodiscard]] auto handler_for( const std::string_view name ) const
			-> const tool_handler* {
			const auto found = handlers_.find( std::string{ name } );

			return found != handlers_.end( ) ? &found->second : nullptr;
		}

		// a plan response with no tool call is the turn's answer, so Act never re-requests it.
		[[nodiscard]] auto run( const std::string_view user_task ) -> result< turn_outcome >;

		// A cancel source the caller owns and may set from another thread. The
		// loop reads it at each step boundary and stops the turn there, which is
		// the honest bound: a model request already in flight is not abandoned
		// mid-stream, because the transport has no way to take that back.
		//
		// Without this the REPL's Ctrl+C and Esc only stopped *displaying* the
		// answer -- the turn ran to completion on the worker thread, so the
		// session looked interrupted and was not, and the committed banner's
		// "Ctrl+C to interrupt" was false.
		auto set_cancel_source( const std::atomic< bool >* value ) -> void {
			cancel_ = value;
		}

		[[nodiscard]] auto budget( ) const noexcept -> const session_budget& { return budget_; }

		// The loop thread stays the only publisher.
		[[nodiscard]] auto bus( ) const noexcept -> events::bus& { return *bus_; }
		[[nodiscard]] auto budget( ) noexcept -> session_budget& { return budget_; }
		[[nodiscard]] auto log( ) const noexcept -> const event_log& { return *log_; }

		[[nodiscard]] auto registry( ) const noexcept -> const tool_registry& {
			return *registry_;
		}

		// Empty means none configured, which routes Verify to Handoff.
		auto set_verification_command( const std::string command ) -> void {
			verification_command_ = std::move( command );
		}

		[[nodiscard]] auto state( ) const noexcept -> loop_state { return state_; }

		[[nodiscard]] auto model_name( ) const noexcept -> std::string_view {
			return model_name_;
		}

		// the same counter the budget charges; the atomic load lets the REPL read it mid-turn.
		[[nodiscard]] auto context_used( ) const noexcept -> std::uint64_t {
			return budget_.tokens_used.load( );
		}

		// The model's context window in tokens; 0 means unknown, so a caller omits the ratio.
		[[nodiscard]] auto context_capacity( ) const noexcept -> std::uint64_t {
			return caps_.context_window > 0
				? static_cast< std::uint64_t >( caps_.context_window )
				: 0;
		}

		[[nodiscard]] auto history( ) const noexcept -> const std::vector< model::message >& {
			return history_;
		}

		// Replaces the conversation with a restored one, for `--continue` and
		// `/resume`. The transcript is carried into the next turn so the model
		// continues the work rather than starting from an empty history.
		auto seed_history( std::vector< model::message > restored ) -> void;

		// Adopts a different session in place: the conversation becomes `restored`
		// and the budget counters start again from zero.
		//
		// `seed_history` alone is not enough for a swap. Three things are read
		// BETWEEN turns, and all three would otherwise still describe the session
		// the user just left:
		//
		//  - `run_id()` is the capture group `/undo` restores. Minted only at the
		//    top of `run()`, so after a swap it names the previous session's last
		//    run and `/undo` would revert files the user did not ask about.
		//  - `budget()` is cumulative and `run()` never resets it, so `/cost` and
		//    the status meter would report the previous session's spend, and a
		//    session that had already spent its budget would refuse the first turn.
		//  - `state()` still reports the previous session's terminal state.
		//
		// The budget LIMITS are kept: those come from the command line and describe
		// this process, not the session. A resumed session's prior spend is not in
		// the log, so starting the counters at zero is the honest reading rather
		// than a guess.
		auto reset_session( std::vector< model::message > restored ) -> void;

		// The approval mode the engine is running under (`never`, `on-request`,
		// `always`), for a UI that has to state it. Empty when no engine is wired.
		[[nodiscard]] auto approval_mode( ) const noexcept -> std::string_view;

		// The project instruction chain the session loaded; empty when none was found.
		[[nodiscard]] auto instruction_chain( ) const noexcept -> std::string_view {
			return instruction_chain_;
		}

		// The canonicalized workspace root the session was opened on; honours --cwd.
		[[nodiscard]] auto workspace_root( ) const noexcept -> std::string_view {
			return workspace_root_;
		}

		// Null when the caller supplied no store: /undo then reports that nothing was captured.
		[[nodiscard]] auto snapshots( ) const noexcept -> snapshot_store* { return snapshots_; }

		// Identifies this run's captures in the snapshot index; empty before the first run.
		[[nodiscard]] auto run_id( ) const noexcept -> std::string_view {
			return snapshot_run_id_;
		}

		// The run-level exit-5 signal: a denial that leaves the loop unable to progress.
		[[nodiscard]] auto permission_denied( ) const noexcept -> bool {
			return permission_denied_;
		}

		auto observe_result( const tool_call& call, const tool_outcome& outcome ) -> void;

		auto finish_run( const loop_state terminal, const std::string_view reason ) -> void;

	private:
		// drives plan -> act -> observe -> verify, reflecting or replanning when it stalls.
		auto run_state_machine( ) -> turn_outcome;

		auto request_and_fold( model::effort effort ) -> result< bool >;

		// Clears pending_calls_ and dispatches them, recording a hard error for observe.
		auto dispatch_pending( ) -> void;

		// Discards a response whose tool arguments cannot be repaired and asks the model again.
		auto resample_pending_calls( ) -> bool;

		auto dispatch_calls( const std::vector< tool_call >& calls ) -> bool;

		// Refuses an exact repeat already dispatched DOOM_LOOP_THRESHOLD times.
		[[nodiscard]] auto refuse_doom_loop( const tool_call& call, const std::size_t repeats )
			-> tool_outcome;

		// Records the write target's current bytes before the handler runs; a failure is
		// logged and swallowed, because an unusable store must not block a working edit.
		auto capture_before_write( const tool_call& call, const tool_class klass ) -> void;

		// Opens a fresh capture group, so /undo restores this run and not the previous one.
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
		model::provider_descriptor provider_;
		std::string api_key_;
		std::string workspace_root_;
		std::string platform_name_;
		snapshot_store* snapshots_ = nullptr;
		std::string snapshot_run_id_;

		thrash_detector thrash_;
		std::string last_failure_;
		std::string last_failure_class_;
		std::string end_reason_;

		// Repetition-loop guard. `previous_texts_` is the assistant prose of this
		// run, used for the cross-turn comparison; it is cleared with the run.
		std::vector< std::string > previous_texts_;
		std::size_t repetition_steers_ = 0;

		// The assistant text of the response that was just folded, before the
		// guard possibly discards it.
		std::string last_response_text_;

		// Discards the last assistant message and re-asks with a corrective
		// steer. Returns false when the steer budget is spent, so the caller
		// falls through to its normal failure handling rather than looping.
		[[nodiscard]] auto steer_repetition( const agent::repetition_verdict& verdict ) -> bool;

		// the provider's own finish reason for the last response, and whether it means cut off.
		std::string stop_reason_;
		bool turn_truncated_ = false;

		// resamples spent on this run, bounded by MAX_TOOL_CALL_RETRIES.
		std::size_t tool_call_retries_ = 0;

		// Provider-reported token counts summed across the run, so the summary can report
		// cache reads and writes as well as the two totals.
		model::usage run_usage_;

		bool hard_error_ = false;
		bool permission_denied_ = false;
		std::string verification_command_;
		perm::permission_engine* permissions_ = nullptr;
		std::string instruction_chain_;
		std::string skill_index_;
		std::function< void( ) > pump_timers_;
		const std::atomic< bool >* cancel_ = nullptr;
		std::vector< tool_call > pending_calls_;

		// the plan response carried no tool call, so Act reuses it as the turn's answer.
		bool plan_answered_ = false;
		std::map< std::string, std::size_t, std::less<> > reflection_counts_;
		std::size_t total_reflections_ = 0;
		std::size_t replan_count_ = 0;
		std::size_t last_failure_repeats_ = 0;

		std::map< std::string, tool_handler, std::less<> > handlers_;
	};

} // namespace mcode
