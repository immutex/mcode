#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/fs/workspace.hxx"

namespace mcode::tools {

	// ~2K tokens at ~4 chars/token
	inline constexpr std::size_t INLINE_RESULT_CHARS = 8u * 1024u;

	// beyond this even the preview is cut
	inline constexpr std::size_t HARD_RESULT_CHARS = 100u * 1024u;

	inline constexpr std::size_t SPILL_PREVIEW_CHARS = 2u * 1024u;

	// .mcode/ is protected against the model's tools, not against this harness write
	[[nodiscard]] auto truncate_result( std::string_view text, std::string_view run_id,
		workspace& space, std::string_view tool_name ) -> std::string;

	[[nodiscard]] auto needs_truncation( std::string_view text ) noexcept -> bool;

}
