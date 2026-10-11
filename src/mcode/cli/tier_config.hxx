#pragma once

#include <string_view>

#include "mcode/agent/loop.hxx"
#include "mcode/core/error.hxx"
#include "mcode/support/config.hxx"

namespace mcode::cli {

	struct tier_set {
		mcode::tier_model plan;
		mcode::tier_model act;
		mcode::tier_model side;
	};

	// Reads `model.tier.plan|act|side`, each falling back to `default_model`. An
	// unset tier is the common case. A tier naming an unpriced model fails closed:
	// it would report $0.00 and silently defeat the budget.
	[[nodiscard]] auto resolve_tiers( std::string_view default_model,
		const mcode::config::merged_config& config ) -> mcode::result< tier_set >;

}
