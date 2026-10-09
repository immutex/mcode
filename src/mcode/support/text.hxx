#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::text {

	// The bytes-per-token estimate every budget in this program divides by.
	// It was declared three times under three names -- here, in `agent/loop.hxx`
	// and in `mcp/constants.hxx` -- so `/doctor`, the instruction-chain budget and
	// the MCP schema warning could each report a different token count for the
	// same bytes.
	inline constexpr std::size_t CHARS_PER_TOKEN = 4;

	// ASCII-only, locale-independent folding.
	//
	// `std::tolower` takes the current locale into account, which makes it a
	// different function in a Turkish locale for the letters `I` and `i` -- and
	// the permission layer compares tool and program names with this, so two
	// spellings of "fold" there is a security hazard rather than untidiness.
	// Twelve hand-written copies existed across the tree.
	[[nodiscard]] auto ascii_lower( std::string_view input ) -> std::string;

	[[nodiscard]] auto is_ascii_space( char character ) noexcept -> bool;

	// The executable name from an argv[0]-style path: directories stripped and
	// folded, so `C:\Program Files\Git\rm.exe` and `./rm` both answer `rm.exe`
	// and `rm`. Three hand-written copies existed in `perm/`, which is where the
	// permission floor decides what a command IS -- they have to agree.
	[[nodiscard]] auto program_basename( std::string_view program ) -> std::string;

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
