#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/types.hxx"

namespace mcode {

	namespace model {

		struct message;

	}

	struct context_occupancy {
		// Last request's prompt; window occupancy, NOT cumulative spend.
		std::int64_t prompt_tokens = 0;

		std::int64_t usable_window = 0;

		std::int64_t cached_read = 0;
		std::int64_t cache_write = 0;

		std::uint32_t epoch = 0;

		[[nodiscard]] auto fill_fraction( ) const noexcept -> double;
		[[nodiscard]] auto cache_hit_ratio( ) const noexcept -> double;
	};

	enum class context_action {
		none,
		// Old tool results replaced with restorable stubs; the call records stay.
		clear,
	};

	struct context_plan {
		context_action action = context_action::none;
		std::size_t keep_last_results = 0;
	};

	[[nodiscard]] auto plan_context( const std::vector< model::message >& history,
		const context_occupancy& occupancy, std::int64_t keep_last_tokens ) -> context_plan;

	// The assistant call that produced each result is left alone, so the model can
	// still re-issue the read.
	[[nodiscard]] auto apply_clear( std::vector< model::message >& history, std::size_t keep )
		-> std::int64_t;

	// Regenerated from session state rather than from a summary, so it is byte-stable
	// between rewrites and sits at the front of the prompt.
	struct pinned_state {
		std::string task;
		std::vector< std::string > modified_files;
		std::string plan;
		std::vector< std::string > open_errors;
	};

	[[nodiscard]] auto render_pinned_state( const pinned_state& state ) -> std::string;

	// A marker for a dropped payload, not for an empty one.
	inline constexpr std::string_view CLEARED_RESULT_PREFIX = "[cleared ";

	inline constexpr double TOOL_CLEAR_TRIGGER_FRACTION = 0.60;

	// A clear below this share of the window never repays the cache-invalidation penalty.
	inline constexpr double CLEAR_AT_LEAST_FRACTION = 0.20;

}
