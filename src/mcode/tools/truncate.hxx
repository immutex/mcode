#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/fs/workspace.hxx"

namespace mcode::tools {

	// Inline cap for a tool result, ~2K tokens at ~4 chars/token (docs/05 owns the
	// number).
	inline constexpr std::size_t INLINE_RESULT_CHARS = 8u * 1024u;

	// Absolute hard cap. Beyond this even the preview is cut, with a resume hint
	// naming the next call.
	inline constexpr std::size_t HARD_RESULT_CHARS = 100u * 1024u;

	// Characters of the spilled text shown inline alongside the artifact path.
	inline constexpr std::size_t SPILL_PREVIEW_CHARS = 2u * 1024u;

	// Truncates a tool result to the inline budget, spilling the full text to
	// `.mcode/artifacts/<run-id>/` when it does not fit.
	//
	// The spill goes through `workspace::write_file`, so it is atomic and inside
	// the workspace. `.mcode/` is protected against the MODEL's tools; this is the
	// harness writing, which is exactly the actor split the protection query
	// exists to express. `run_id` scopes the directory so two runs cannot collide
	// and a replayed session's references still resolve. The file name carries a
	// per-call sequence number, so two truncated results from the same tool in one
	// run never collide.
	//
	// The returned text is always within INLINE_RESULT_CHARS and never a bare
	// ellipsis: the notice names what was cut and the artifact path that holds
	// the whole output.
	[[nodiscard]] auto truncate_result( std::string_view text, std::string_view run_id,
		workspace& space, std::string_view tool_name ) -> std::string;

	// True when `text` needs truncation at all.
	[[nodiscard]] auto needs_truncation( std::string_view text ) noexcept -> bool;

}
