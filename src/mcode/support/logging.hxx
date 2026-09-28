#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode {

	enum class log_level { trace, debug, info, warn, error, critical, off };

	// Absent when the name is not a level, so a caller can fail closed on a typo
	// rather than silently getting the default.
	[[nodiscard]] auto parse_log_level( std::string_view name ) -> std::optional< log_level >;
	[[nodiscard]] auto to_string( const log_level level ) noexcept -> std::string_view;

	// Returns a failure when a requested log directory could not be created. The
	// stdout sink is still installed, so the caller can log the problem -- but it
	// learns about it, which a silent fallback would not tell it.
	auto init_logging( const log_level level, const std::filesystem::path& log_directory = { } )
		-> status;
	auto shutdown_logging( ) -> void;

	[[nodiscard]] auto logging_initialized( ) noexcept -> bool;
	[[nodiscard]] auto logger( ) noexcept -> std::shared_ptr< spdlog::logger >;

}
