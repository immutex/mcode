#pragma once

#include <optional>
#include <string>

#include "mcode/core/registry.hxx"

namespace mcode::perm {

	struct rule;

	// Whether one rule matches one request, per the rule grammar: exec rules
	// match the canonical argv exactly (never a prefix), read and write rules
	// glob over the canonical path, and the remaining classes match the tool
	// name exactly.
	[[nodiscard]] auto rule_matches( const rule& candidate,
		const tool_class request_class, const std::string& request_resource,
		const std::string& request_tool_name ) -> bool;

	// The default-set deny rules for secret material: reading `.env`, key
	// files, `id_rsa*`, `.aws/` and `*.pem` is denied by the default set, the
	// way the resolved table lists them. The resource must be the canonical
	// path. Nothing means the rule set does not fire and the caller falls
	// through to the ordinary default decision.
	[[nodiscard]] auto default_secret_deny( const std::string& canonical_path )
		-> std::optional< std::string >;

}
