#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"

namespace mcode {

	inline constexpr std::uint32_t DEFAULT_MAX_STEPS = 100;
	inline constexpr std::uint64_t DEFAULT_MAX_TOKENS = 2'000'000;
	inline constexpr double DEFAULT_MAX_USD = 5.0;
	inline constexpr double NEARLY_EXHAUSTED_FRACTION = 0.20;

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

	class event_log {
	public:
		auto append( std::string kind, std::string payload_json = "{}" ) -> event;

		[[nodiscard]] auto events( ) const noexcept -> const std::vector< event >& { return events_; }
		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return events_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return events_.empty( ); }

		[[nodiscard]] auto branch_id( ) const noexcept -> const std::string& { return branch_id_; }

		auto set_branch_id( std::string id ) -> void { branch_id_ = std::move( id ); }

		[[nodiscard]] auto to_jsonl( ) const -> std::string;

	private:
		std::vector< event > events_;
		std::uint64_t next_sequence_ = 0;
		std::string branch_id_;
	};

	struct tool_call {
		std::string name;
		std::string args_json;
	};

	struct tool_outcome {
		bool ok = false;
		std::string content;
		errc code = errc::ok;
		std::string error_message;
		std::chrono::milliseconds elapsed{ 0 };
	};

	class agent_loop {
	public:
		using tool_handler = std::function< result< std::string >( std::string_view args_json ) >;

		agent_loop( tool_registry& registry, event_log& log, session_budget budget = { } )
			: registry_( registry ), log_( log ), budget_( budget ) { }

		[[nodiscard]] auto execute( const tool_call& call ) -> tool_outcome;

		auto register_handler( std::string name, tool_handler handler ) -> void;

		[[nodiscard]] auto budget( ) const noexcept -> const session_budget& { return budget_; }
		[[nodiscard]] auto budget( ) noexcept -> session_budget& { return budget_; }
		[[nodiscard]] auto log( ) const noexcept -> const event_log& { return log_; }

	private:
		tool_registry& registry_;
		event_log& log_;
		session_budget budget_;
		std::vector< std::pair< std::string, tool_handler > > handlers_;
	};

}
