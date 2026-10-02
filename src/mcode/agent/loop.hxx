#pragma once

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

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/model/client.hxx"
#include "mcode/model/types.hxx"

namespace mcode::perm {
	class permission_engine;
}

namespace mcode {

	inline constexpr std::uint32_t DEFAULT_MAX_STEPS = 100;
	inline constexpr std::uint64_t DEFAULT_MAX_TOKENS = 2'000'000;
	inline constexpr double DEFAULT_MAX_USD = 5.0;
	inline constexpr double NEARLY_EXHAUSTED_FRACTION = 0.20;

	inline constexpr std::size_t THRASH_WINDOW = 12;
	inline constexpr std::size_t THRASH_REPEAT_LIMIT = 3;

	inline constexpr std::size_t MAX_REFLECTIONS_PER_FAILURE_CLASS = 2;
	inline constexpr std::size_t MAX_REFLECTIONS_PER_RUN = 4;

	inline constexpr std::size_t THRASH_ESCALATION_FACTOR = 2;
	inline constexpr std::size_t REPLAN_GUARD_LIMIT = 2;
	inline constexpr std::size_t REFLECT_REPEAT_LIMIT = 2;

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

	struct session_budget {
		std::uint32_t max_steps = DEFAULT_MAX_STEPS;
		std::uint64_t max_tokens = DEFAULT_MAX_TOKENS;
		double max_usd = DEFAULT_MAX_USD;

		std::uint32_t steps_used = 0;
		std::uint64_t tokens_used = 0;
		double usd_used = 0.0;

		[[nodiscard]] auto exhausted( ) const noexcept -> bool {
			return steps_used >= max_steps || tokens_used >= max_tokens || usd_used >= max_usd;
		}

		[[nodiscard]] auto nearly_exhausted( ) const noexcept -> bool {
			if ( max_usd <= 0.0 ) {
				return exhausted( );
			}

			return ( 1.0 - usd_used / max_usd ) < NEARLY_EXHAUSTED_FRACTION;
		}

		auto charge( const std::uint64_t tokens, const double usd ) noexcept -> void {
			++steps_used;
			tokens_used += tokens;
			usd_used += usd;
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
		[[nodiscard]] auto write_failures( ) const noexcept -> std::uint64_t { return write_failures_; }

		auto append( const std::string kind, std::string payload_json = "{}" ) -> event;

		// Not append(): this preserves the recorded sequence and must not renumber.
		auto restore( event recorded ) -> void;

		[[nodiscard]] auto events( ) const noexcept -> const std::vector< event >& { return events_; }
		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return events_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return events_.empty( ); }


		[[nodiscard]] auto next_sequence( ) const noexcept -> std::uint64_t { return next_sequence_; }


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
	};

	struct tool_outcome {
		bool ok = false;
		std::string content;
		errc code = errc::ok;
		std::string error_message;
		std::chrono::milliseconds elapsed{ 0 };

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

		std::string summary_json;
		std::size_t model_calls = 0;
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
		auto record( const std::string_view tool_name, const std::string_view args_json ) -> std::size_t;

		[[nodiscard]] auto repeat_count( ) const noexcept -> std::size_t { return current_repeats_; }

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

		[[nodiscard]] auto run( const std::string_view user_task ) -> result< turn_outcome >;

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
		[[nodiscard]] auto history( ) const noexcept -> const std::vector< model::message >& {
			return history_;
		}

		// The run-level exit-5 signal: a denial that leaves the loop unable to progress.
		[[nodiscard]] auto permission_denied( ) const noexcept -> bool {
			return permission_denied_;
		}

		auto observe_result( const tool_call& call, const tool_outcome& outcome ) -> void;

		auto finish_run( const loop_state terminal, const std::string_view reason ) -> void;

	private:
		auto run_state_machine( ) -> turn_outcome;
		auto request_and_fold( model::effort effort ) -> result< bool >;
		auto dispatch_calls( const std::vector< tool_call >& calls ) -> bool;
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

		thrash_detector thrash_;
		std::string last_failure_;
		bool hard_error_ = false;
		bool permission_denied_ = false;
		std::string verification_command_;
		perm::permission_engine* permissions_ = nullptr;
		std::string instruction_chain_;
		std::string skill_index_;
		std::vector< tool_call > pending_calls_;
		std::map< std::string, std::size_t, std::less<> > reflection_counts_;
		std::size_t total_reflections_ = 0;
		std::size_t replan_count_ = 0;
		std::size_t last_failure_repeats_ = 0;

		std::map< std::string, tool_handler, std::less<> > handlers_;
	};

} // namespace mcode
