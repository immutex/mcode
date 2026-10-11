#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mcode/perm/approval.hxx"
#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	// Nothing defaults to allow. Closed input resolves to `refused`, which the
	// engine treats as deny, and every other answer is re-presented.
	class ui_approval_source final : public perm::approval_source {
	public:
		// An empty queue models closed input: the next ask is refused.
		using answer_queue = std::deque< std::string >;

		// A nullopt from the live reader is closed input, resolved as `refused`.
		using answer_source = std::function< std::optional< std::string >( ) >;

		struct prompt_view {
			std::vector< styled_line > rows;

			// Why the last answer was not taken; empty on the first presentation.
			std::string notice;

			// The `?` detail view, for the transcript above the block.
			std::string above;
		};

		// `ask` blocks on input, so without a presenter nothing is drawn.
		using presenter = std::function< void( const prompt_view& ) >;

		// Called on every path out of `ask`: a block left up suppresses the pump.
		using dismisser = std::function< void( ) >;

		explicit ui_approval_source( answer_queue& answers );
		ui_approval_source( answer_source source, presenter present, dismisser dismiss );

		[[nodiscard]] auto ask( const perm::approval_request& request,
			const std::function< std::string( ) >& detail ) -> perm::approval_outcome override;

		[[nodiscard]] auto render( const perm::approval_request& request ) const
			-> std::vector< styled_line >;

		[[nodiscard]] auto asks( ) const noexcept -> std::size_t { return asks_; }
		[[nodiscard]] auto details_shown( ) const noexcept -> std::size_t {
			return details_;
		}

	private:
		answer_queue* answers_ = nullptr;
		answer_source source_;
		presenter present_;
		dismisser dismiss_;
		std::size_t asks_ = 0;
		std::size_t details_ = 0;
	};

}
