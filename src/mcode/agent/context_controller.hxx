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

	// Context is a degrading resource, not a gauge. Two numbers describe a turn and
	// the current build conflates them: what the run has spent, and how full the
	// window actually is.
	struct context_occupancy {
		// The last request's prompt: input plus cache reads plus cache writes. This
		// is window occupancy. It is NOT cumulative spend, which is what the meter
		// compared against the window before this existed, so a long session read
		// over 100%.
		std::int64_t prompt_tokens = 0;

		// The window minus the output reserve, floor-clamped.
		std::int64_t usable_window = 0;

		std::int64_t cached_read = 0;
		std::int64_t cache_write = 0;

		// Bumped by every rewrite, so a consumer can see one as a decision rather
		// than as a mysteriously expensive turn.
		std::uint32_t epoch = 0;

		[[nodiscard]] auto fill_fraction( ) const noexcept -> double;
		[[nodiscard]] auto cache_hit_ratio( ) const noexcept -> double;
	};

	// What the controller decided for this step. `none` is the common case.
	enum class context_action {
		none,
		// Old tool results replaced with restorable stubs; the call records stay.
		clear,
	};

	// The decision as data: a tag and a count, never a rebuilt history.
	struct context_plan {
		context_action action = context_action::none;
		std::size_t keep_last_results = 0;
	};

	// Pure functions over the history, so the policy is unit-testable without a
	// loop, a model or a log.
	[[nodiscard]] auto plan_context( const std::vector< model::message >& history,
		const context_occupancy& occupancy, std::int64_t keep_last_tokens ) -> context_plan;

	// Replaces tool results older than the newest `keep` with a restorable stub.
	// The assistant call that produced each one is left alone, so the model can
	// still see what it asked for and re-issue the read.
	[[nodiscard]] auto apply_clear( std::vector< model::message >& history, std::size_t keep )
		-> std::int64_t;

	// The pinned block: regenerated from session state rather than from a summary,
	// so it is byte-stable between rewrites and sits at the front of the prompt --
	// the position that is actually attended to.
	struct pinned_state {
		std::string task;
		std::vector< std::string > modified_files;
		std::string plan;
		std::vector< std::string > open_errors;
	};

	[[nodiscard]] auto render_pinned_state( const pinned_state& state ) -> std::string;

	// What a cleared tool result leaves behind: enough to re-fetch, and a marker
	// that says the payload was dropped rather than that it was empty.
	inline constexpr std::string_view CLEARED_RESULT_PREFIX = "[cleared ";

	// Rung 1 of the ladder, owned here because this is where the policy lives.
	// Clearing is what does most of the work: 84% of a turn's tokens are
	// environment observations, and masking them matches summarization's solve
	// rate at roughly half the cost.
	inline constexpr double TOOL_CLEAR_TRIGGER_FRACTION = 0.60;

	// The break-even. A rewrite costs ~1.15x of the rewritten region once and saves
	// ~0.9x per later turn, so a clear below this share of the window pays the
	// cache-invalidation penalty for a saving that never lands. Cut deep, cut once.
	inline constexpr double CLEAR_AT_LEAST_FRACTION = 0.20;

}
