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

	// The approval prompt rendered in the live region, answered from the
	// input layer. The engine's semantics are kept exactly:
	//
	//   - `detail` calls the detail callback and re-prompts;
	//   - an unrecognised answer re-prompts;
	//   - closed input returns `refused`, which the engine resolves as deny.
	//
	// Nothing defaults to allow. A prompt that guesses is a prompt that
	// grants by accident.
	//
	// Every re-prompt presents again. A silent loop looks exactly like a hung
	// session, and a user who cannot see that their answer was rejected will
	// type it again, forever.
	class ui_approval_source final : public perm::approval_source {
	public:
		// The answers are injected, so tests drive the prompt without a
		// terminal. An empty queue models closed input: the next ask is
		// refused.
		using answer_queue = std::deque< std::string >;

		// The live path: answers come from the raw-mode line reader. A
		// nullopt is closed input and resolves to `refused`.
		using answer_source = std::function< std::optional< std::string >( ) >;

		// What the presenter draws.
		struct prompt_view {
			// The request's own rows, from `render`.
			std::vector< styled_line > rows;

			// One row above the question: why the last answer was not taken.
			// Empty on the first presentation.
			std::string notice;

			// Text that belongs in the transcript above the block, which is
			// where the `?` detail view goes. Empty when there is none.
			std::string above;
		};

		// Draws the prompt rows before the answer is read. `ask` blocks on
		// input, and nothing else draws the request, so without a presenter
		// the session froze with no indication of what was being asked.
		using presenter = std::function< void( const prompt_view& ) >;

		// Called once when the question stops owning the console, on every
		// path out of `ask`: an answer, a denial, or closed input. A block
		// left up suppresses the repaint pump for the rest of the turn.
		using dismisser = std::function< void( ) >;

		explicit ui_approval_source( answer_queue& answers );
		ui_approval_source( answer_source source, presenter present, dismisser dismiss );

		[[nodiscard]] auto ask( const perm::approval_request& request,
			const std::function< std::string( ) >& detail ) -> perm::approval_outcome override;

		// The rendered prompt rows, for the renderer and for tests.
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
