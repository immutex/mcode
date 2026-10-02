#pragma once

#include <functional>
#include <optional>
#include <string>

namespace mcode::perm {

	// deny_session outranks later allows; detail asks for the `[?]` view and re-prompts.
	enum class approval_outcome {
		allow_once,
		allow_remember,
		deny_once,
		deny_session,
		detail,
		refused,
	};

	// fully rendered by the engine: the source is presentation, it asks but does not decide.
	struct approval_request {
		std::string action;

		// parsed argv joined with single spaces for exec, the canonical path for file tools.
		std::string subject;

		std::string rule;

		std::string working_directory;
	};

	class approval_source {
	public:
		virtual ~approval_source( ) = default;

		// refused means no answer was obtainable, which the engine resolves as deny.
		[[nodiscard]] virtual auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome = 0;
	};

}

