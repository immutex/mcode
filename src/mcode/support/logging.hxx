#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace mcode {

	enum class log_level { trace, debug, info, warn, error, critical, off };

	[[nodiscard]] auto parse_log_level( const std::string_view name ) noexcept -> log_level;
	[[nodiscard]] auto to_string( const log_level level ) noexcept -> std::string_view;

	auto init_logging( const log_level level, const std::filesystem::path& log_directory = { } ) -> void;
	auto shutdown_logging( ) -> void;

	[[nodiscard]] auto logging_initialized( ) noexcept -> bool;
	[[nodiscard]] auto logger( ) noexcept -> std::shared_ptr< spdlog::logger >;

}
