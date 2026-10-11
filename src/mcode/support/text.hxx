#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::text {

	// the divisor that turns a byte count into an estimated token count.
	inline constexpr std::size_t CHARS_PER_TOKEN = 4;

	// ASCII-only and locale-independent; `std::tolower` varies with the locale,
	// and the permission layer compares names with this.
	[[nodiscard]] auto ascii_lower( std::string_view input ) -> std::string;

	// folded as well as stripped, so `rm.exe` and `./rm` answer `rm.exe` and `rm`.
	[[nodiscard]] auto program_basename( std::string_view program ) -> std::string;

	// a byte count as a reader compares it, `12 MB` not `12288 KB`.
	[[nodiscard]] auto human_bytes( std::uint64_t bytes ) -> std::string;



	[[nodiscard]] auto is_valid_utf8( std::string_view input ) noexcept -> bool;
	[[nodiscard]] auto codepoint_count( std::string_view input ) noexcept -> std::size_t;

	[[nodiscard]] auto truncate_offset( std::string_view input, std::size_t max_bytes ) noexcept
		-> std::size_t;

	[[nodiscard]] auto truncate( std::string_view input, std::size_t max_bytes,
		std::string_view ellipsis = "..." ) -> std::string;

	[[nodiscard]] auto sanitize_utf8( std::string_view input ) -> std::string;

	// Counts codepoints that render as nothing: Cc and Cf (except TAB, LF, CR) and
	// the variation selectors U+FE00..FE0F, U+E0100..E01EF. A payload hidden in
	// these is invisible in every editor and diff view.
	[[nodiscard]] auto count_invisible( std::string_view input ) noexcept -> std::size_t;

	[[nodiscard]] auto count_invisible( const std::filesystem::path& directory ) -> std::size_t;

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
