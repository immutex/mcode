#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::tools {

	// One reason a call cannot satisfy the tool's declared schema. The model is told the
	// parameter, what was observed, and - when the schema bounds the domain - the alternatives,
	// because naming the admissible values is what recovers a call the model can repair itself.
	struct argument_fault {
		std::string parameter;
		std::string problem;
		std::string admissible;
	};

	// Checks a parsed argument object against the tool's declared schema: required members,
	// primitive types, and enum membership. A schema that does not constrain a value passes it.
	// An unparsable schema is an error; unparsable arguments are the caller's to repair first.
	[[nodiscard]] auto check_arguments( std::string_view schema_json, std::string_view args_json )
		-> result< std::vector< argument_fault > >;

	// Renders the faults as one message: `path: is required but missing; limit: ...`.
	[[nodiscard]] auto describe_faults( const std::vector< argument_fault >& faults )
		-> std::string;

}
