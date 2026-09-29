#pragma once

#include <functional>
#include <optional>
#include <string>

namespace mcode::perm {

	// What the user answered at an approval prompt. `remember` persists to the
	// store; `deny_session` records a session-scope deny that outranks later
	// allows; `detail` asks for the `[?]` view and re-prompts.
	enum class approval_outcome {
		allow_once,
		allow_remember,
		deny_once,
		deny_session,
		detail,
		refused,
	};

	// One pending approval, fully rendered by the engine before the source
	// sees it. The source is presentation: it asks, it does not decide.
	struct approval_request {
		// The action in progress, e.g. "run" for exec, "write" for file tools.
		std::string action;

		// The canonical subject: parsed argv joined with single spaces for
		// exec, the canonical path for file tools, the tool name for mcp.
		std::string subject;

		// Which rule asked, in `scope: rule` form, or the default-set note.
		std::string rule;

		// The workspace root and working directory at the time of the call.
		std::string working_directory;
	};

	// The approval interface. Three implementations: terminal (interactive),
	// headless (fail-closed, never prompts), scripted (tests).
	class approval_source {
	public:
		virtual ~approval_source( ) = default;

		// Asks the user. Returns `refused` when no answer is obtainable --
		// closed stdin, no terminal -- which the engine resolves as deny.
		// The detail callback serves the `[?]` view: the source may call it
		// once before answering, to show what is about to run.
		[[nodiscard]] virtual auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome = 0;
	};

}

