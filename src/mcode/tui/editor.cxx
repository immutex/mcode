#include "mcode/tui/editor.hxx"

#include <algorithm>

namespace mcode::tui {

	namespace {

		// The paste placeholder: a bracketed-paste blob renders as the marker
		// so a token dump never floods the live region.
		inline constexpr std::string_view PASTE_MARKER = "[pasted text]";

		// A line longer than this renders as the marker instead of itself.
		inline constexpr std::size_t PASTE_RENDER_LIMIT = 256;

		// The ghost-text corpus is the session's own history: the first
		// history entry that starts with the current line suggests the rest.
		[[nodiscard]] auto longest_prefix_suggestion(
			const std::vector< std::string >& history, const std::string_view current )
			-> std::string {
			if ( current.empty( ) ) {
				return { };
			}

			for ( auto index = history.size( ); index > 0; --index ) {
				const auto& entry = history[ index - 1 ];

				if ( entry.size( ) > current.size( ) &&
					entry.starts_with( current ) ) {
					return entry.substr( current.size( ) );
				}
			}

			return { };
		}

	}

	auto input_editor::handle( const key_event& event ) -> std::optional< std::string > {
		auto& line = lines_[ cursor_row_ ];

		switch ( event.type ) {
			case key::character: {
				line.text.insert( line.cursor, event.text );
				line.cursor += event.text.size( );

				return std::nullopt;
			}

			case key::enter: {
				auto submission = std::string{ };

				for ( auto index = std::size_t{ 0 }; index < lines_.size( ); ++index ) {
					if ( index > 0 ) {
						submission += '\n';
					}

					submission += lines_[ index ].text;
				}

				if ( !submission.empty( ) ) {
					push_history( submission );
				}

				reset( );

				return submission;
			}

			case key::shift_enter:
			case key::backslash_newline: {
				auto split = editor_line{ };
				split.text = line.text.substr( line.cursor );

				line.text.resize( line.cursor );

				lines_.insert( lines_.begin( ) +
					static_cast< std::ptrdiff_t >( cursor_row_ + 1 ), std::move( split ) );
				++cursor_row_;

				return std::nullopt;
			}

			case key::backspace: {
				if ( line.cursor > 0 ) {
					--line.cursor;
					line.text.erase( line.cursor, 1 );

					return std::nullopt;
				}

				if ( cursor_row_ > 0 ) {
					// Join with the previous line.
					const auto previous = cursor_row_ - 1;
					const auto moved = lines_[ cursor_row_ ].text;

					lines_[ previous ].text += moved;
					lines_.erase( lines_.begin( ) +
						static_cast< std::ptrdiff_t >( cursor_row_ ) );
					cursor_row_ = previous;
				}

				return std::nullopt;
			}

			case key::delete_key: {
				if ( line.cursor < line.text.size( ) ) {
					line.text.erase( line.cursor, 1 );
				} else if ( cursor_row_ + 1 < lines_.size( ) ) {
					line.text += lines_[ cursor_row_ + 1 ].text;
					lines_.erase( lines_.begin( ) +
						static_cast< std::ptrdiff_t >( cursor_row_ + 1 ) );
				}

				return std::nullopt;
			}

			case key::left: {
				if ( line.cursor > 0 ) {
					--line.cursor;
				} else if ( cursor_row_ > 0 ) {
					--cursor_row_;
					lines_[ cursor_row_ ].cursor = lines_[ cursor_row_ ].text.size( );
				}

				return std::nullopt;
			}

			case key::right: {
				if ( line.cursor < line.text.size( ) ) {
					++line.cursor;
				} else if ( cursor_row_ + 1 < lines_.size( ) ) {
					++cursor_row_;
					lines_[ cursor_row_ ].cursor = 0;
				}

				return std::nullopt;
			}

			case key::up: {
				if ( cursor_row_ > 0 ) {
					--cursor_row_;
					lines_[ cursor_row_ ].cursor =
						std::min( lines_[ cursor_row_ ].cursor, lines_[ cursor_row_ ].text.size( ) );

					return std::nullopt;
				}

				// History navigation.
				if ( history_.empty( ) ) {
					return std::nullopt;
				}

				if ( !history_draft_ ) {
					history_draft_ = lines_[ 0 ].text;
					history_position_ = history_.size( );
				}

				if ( history_position_ > 0 ) {
					--history_position_;
					lines_[ 0 ].text = history_[ history_position_ ];
					lines_[ 0 ].cursor = lines_[ 0 ].text.size( );
				}

				return std::nullopt;
			}

			case key::down: {
				if ( cursor_row_ + 1 < lines_.size( ) ) {
					++cursor_row_;
					lines_[ cursor_row_ ].cursor =
						std::min( lines_[ cursor_row_ ].cursor, lines_[ cursor_row_ ].text.size( ) );

					return std::nullopt;
				}

				if ( !history_draft_ ) {
					return std::nullopt;
				}

				if ( history_position_ + 1 < history_.size( ) ) {
					++history_position_;
					lines_[ 0 ].text = history_[ history_position_ ];
					lines_[ 0 ].cursor = lines_[ 0 ].text.size( );

					return std::nullopt;
				}

				lines_[ 0 ].text = *history_draft_;
				lines_[ 0 ].cursor = lines_[ 0 ].text.size( );
				history_draft_.reset( );
				history_position_ = history_.size( );

				return std::nullopt;
			}

			case key::home: {
				line.cursor = 0;

				return std::nullopt;
			}

			case key::end: {
				line.cursor = line.text.size( );

				return std::nullopt;
			}

			case key::paste: {
				// The blob enters the buffer as truth; render() shows the
				// marker in its place, so a token dump never floods the
				// live region but the submission carries the real text.
				line.text.insert( line.cursor, event.text );
				line.cursor += event.text.size( );

				return std::nullopt;
			}

			case key::interrupt: {
				// Ctrl+C clears the pending input; the session continues.
				reset( );

				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	auto input_editor::reset( ) -> void {
		lines_.assign( 1, editor_line{ } );
		cursor_row_ = 0;
		history_draft_.reset( );
		history_position_ = history_.size( );
	}

	auto input_editor::suggestion( ) const -> std::string {
		if ( lines_.empty( ) ) {
			return { };
		}

		return longest_prefix_suggestion( history_, lines_.back( ).text );
	}

	auto input_editor::push_history( std::string entry ) -> void {
		// The most recent duplicate is dropped so Up walks distinct entries.
		if ( !history_.empty( ) && history_.back( ) == entry ) {
			return;
		}

		history_.push_back( std::move( entry ) );
	}

	auto input_editor::render( ) const -> std::vector< styled_line > {
		auto out = std::vector< styled_line >{ };

		for ( auto index = std::size_t{ 0 }; index < lines_.size( ); ++index ) {
			auto line = styled_line{ };
			line.push_back( { index == 0 ? std::string{ "> " } : std::string{ "  " },
				token::accent } );

			auto body = lines_[ index ].text;

			if ( body.find( '\n' ) != std::string::npos || body.size( ) > PASTE_RENDER_LIMIT ) {
				body = std::string{ PASTE_MARKER };
			}

			line.push_back( { std::move( body ), token::text } );

			out.push_back( std::move( line ) );
		}

		const auto ghost = suggestion( );

		if ( !ghost.empty( ) ) {
			auto line = styled_line{ };
			line.push_back( { std::string{ "  " }, token::muted } );
			line.push_back( { ghost, token::muted, token::none, false, false, true } );

			out.push_back( std::move( line ) );
		}

		return out;
	}

}
