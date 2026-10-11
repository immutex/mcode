#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcode::tui {

	// Named, never raw colours: the depth fallback lives in theme.cxx.
	enum class token : std::uint8_t {
		none,
		text,
		muted,
		accent,
		success,
		warn,
		error,
		thinking,
		code_bg,
		diff_add_bg,
		diff_add_emph,
		diff_del_bg,
		diff_del_emph,
	};

	inline constexpr std::size_t TOKEN_COUNT =
		static_cast< std::size_t >( token::diff_del_emph ) + 1;

	struct style {
		token foreground = token::text;
		token background = token::none;
		bool bold = false;
		bool italic = false;
		bool dim = false;
		bool reverse = false;

		[[nodiscard]] auto operator==( const style& other ) const noexcept -> bool = default;
	};

	// One display column; `width` 0 marks the continuation slot of a wide
	// cluster, which the emitter skips.
	struct cell {
		std::string text;
		style cell_style;
		std::uint8_t width = 1;

		[[nodiscard]] auto operator==( const cell& other ) const noexcept -> bool = default;
	};

	struct styled_span {
		std::string text;
		token color = token::text;
		token background = token::none;
		bool bold = false;
		bool italic = false;
		bool dim = false;
	};

	using styled_line = std::vector< styled_span >;

	[[nodiscard]] auto span_style( const styled_span& value ) noexcept -> style;

	class cell_buffer {
	public:
		cell_buffer( ) = default;
		cell_buffer( std::size_t row_count, std::size_t column_count,
			std::size_t ambiguous_width = 1 );

		[[nodiscard]] auto rows( ) const noexcept -> std::size_t { return rows_; }
		[[nodiscard]] auto columns( ) const noexcept -> std::size_t { return columns_; }
		[[nodiscard]] auto ambiguous_width( ) const noexcept -> std::size_t { return ambiguous_width_; }

		auto resize( std::size_t row_count, std::size_t column_count ) -> void;
		auto clear( ) -> void;

		// unchecked: `row` and `column` must be inside the buffer, multiply included
		[[nodiscard]] auto at( std::size_t row, std::size_t column ) const -> const cell&;
		[[nodiscard]] auto at( std::size_t row, std::size_t column ) -> cell&;

		// writes one cluster, returning the column past it; anything out of range is refused
		auto set_cluster( std::size_t row, std::size_t column, std::string_view cluster_text,
			std::size_t cluster_width, const style& value ) -> std::size_t;

		auto write_text( std::size_t row, std::size_t column, std::string_view text,
			const style& value ) -> std::size_t;

		auto write_line( std::size_t row, const styled_line& line ) -> std::size_t;

	private:
		std::size_t rows_ = 0;
		std::size_t columns_ = 0;
		std::size_t ambiguous_width_ = 1;
		std::vector< cell > cells_;
	};

	// one decoded sequence: its code point and byte length; invalid input is U+FFFD, one byte
	struct utf8_decoded {
		char32_t codepoint = 0xFFFD;
		std::size_t length = 1;
	};

	[[nodiscard]] auto decode_utf8( std::string_view text ) noexcept -> utf8_decoded;

	// the byte length a sequence lead starts: 0 when it cannot start one; continuations unchecked
	[[nodiscard]] auto utf8_lead_length( char lead ) noexcept -> std::size_t;

	// An East Asian ambiguous code point is one column or two per the session's
	// probed width; a mismatch mis-measures every line that contains one.
	[[nodiscard]] auto codepoint_width( char32_t value, std::size_t ambiguous_width ) noexcept
		-> std::size_t;

	// Advances one grapheme cluster: base, extending marks, ZWJ sequences,
	// variation selectors, regional-indicator pairs. One invalid byte is a cluster.
	[[nodiscard]] auto next_cluster( std::string_view text, std::size_t ambiguous_width )
		-> std::pair< std::string_view, std::size_t >;

	[[nodiscard]] auto string_width( std::string_view text, std::size_t ambiguous_width )
		-> std::size_t;

	[[nodiscard]] auto truncate_to_width( std::string_view text, std::size_t max_width,
		std::size_t ambiguous_width ) -> std::string;

}
