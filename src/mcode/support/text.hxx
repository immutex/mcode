#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::text {

	[[nodiscard]] auto is_valid_utf8( std::string_view input ) noexcept -> bool;
	[[nodiscard]] auto codepoint_count( std::string_view input ) noexcept -> std::size_t;

	[[nodiscard]] auto truncate_offset( std::string_view input, std::size_t max_bytes ) noexcept
		-> std::size_t;

	[[nodiscard]] auto truncate( std::string_view input, std::size_t max_bytes,
		std::string_view ellipsis = "..." ) -> std::string;

	[[nodiscard]] auto sanitize_utf8( std::string_view input ) -> std::string;

	[[nodiscard]] auto to_utf16( std::string_view input ) -> result< std::u16string >;
	[[nodiscard]] auto from_utf16( std::u16string_view input ) -> result< std::string >;

	[[nodiscard]] constexpr auto prefer_wide_paths( ) noexcept -> bool {
	#if defined( _WIN32 )
		return true;
	#else
		return false;
	#endif
	}

}
