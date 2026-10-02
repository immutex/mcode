#pragma once

#include <optional>
#include <string>

#include "mcode/core/registry.hxx"

namespace mcode::perm {

	struct rule;

	// exec rules match the canonical argv exactly, never a prefix; path rules glob it.
	[[nodiscard]] auto rule_matches( const rule& candidate,
		const tool_class request_class, const std::string& request_resource,
		const std::string& request_tool_name ) -> bool;

	// the default set's secret denies; the resource must be the canonical path.
	[[nodiscard]] auto default_secret_deny( const std::string& canonical_path )
		-> std::optional< std::string >;

}
