#include "mcode/tui/editor.hxx"

#include <algorithm>

namespace mcode::tui {

	namespace {

		// The length of the backslash run at the end of `text`.
		[[nodiscard]] auto trailing_backslashes( const std::string& text ) -> std::size_t {
			auto count = std::size_t{ 0 };

			while ( count < text.size( ) && text[ text.size( ) - 1 - count ] == '\\' ) {
				++count;
			}

			return count;
		}

	}

	auto input_editor::handle( const key_event& event ) -> std::optional< std::string > {
		auto& line = lines_[ cursor_row_ ];

		// One implementation of "open a row at the caret": the explicit
		// `newline` key and the backslash continuation both go through it.
		const auto open_row = [ & ]( ) {
			auto split = editor_line{ };
			split.text = line.text.substr( line.cursor );
			line.text.resize( line.cursor );

			lines_.insert( lines_.begin( ) +
				static_cast< std::ptrdiff_t >( cursor_row_ + 1 ), std::move( split ) );
			++cursor_row_;
		};

		switch ( event.type ) {
			case key::character: {
				line.text.insert( line.cursor, event.text );
				line.cursor += event.text.size( );

				return std::nullopt;
			}

			case key::enter: {
				// A trailing backslash run is the multi-line gesture: an odd
				// count continues the line with one backslash consumed, an
				// even count submits with one backslash left literal, so
				// `line\` opens a row and `path\\` submits `path\`. Shift+Enter
				// cannot be told apart without the Kitty protocol, which
				// ConPTY does not reliably carry, so this is the gesture.
				const auto trailing = trailing_backslashes( line.text );

				if ( trailing % 2 == 1 ) {
					line.text.pop_back( );
					line.cursor = line.text.size( );
					open_row( );

					return std::nullopt;
				}

				if ( trailing > 0 ) {
					line.text.pop_back( );
					line.cursor = line.text.size( );
				}

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

			case key::newline: {
				open_row( );

				return std::nullopt;
			}

			case key::backspace: {
				if ( line.cursor > 0 ) {
					--line.cursor;
					line.text.erase( line.cursor, 1 );

					return std::nullopt;
				}

				if ( cursor_row_ > 0 ) {
					const auto previous = cursor_row_ - 1;

					lines_[ previous ].text += lines_[ cursor_row_ ].text;
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

			case key::interrupt: {
				reset( );

				return std::nullopt;
			}

			case key::escape: {
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

	auto input_editor::set_text( std::string text ) -> void {
		lines_.assign( 1, editor_line{ std::move( text ), 0 } );
		lines_[ 0 ].cursor = lines_[ 0 ].text.size( );
		cursor_row_ = 0;
	}

	auto input_editor::text( ) const -> std::string {
		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index < lines_.size( ); ++index ) {
			if ( index != 0 ) {
				out.push_back( ' ' );
			}

			out += lines_[ index ].text;
		}

		return out;
	}

	auto input_editor::flattened_cursor( ) const noexcept -> std::size_t {
		auto offset = std::size_t{ 0 };

		for ( auto index = std::size_t{ 0 }; index < lines_.size( ); ++index ) {
			if ( index == cursor_row_ ) {
				return offset + lines_[ index ].cursor;
			}

			offset += lines_[ index ].text.size( ) + 1;
		}

		return offset;
	}

	auto input_editor::push_history( std::string entry ) -> void {
		if ( !history_.empty( ) && history_.back( ) == entry ) {
			return;
		}

		history_.push_back( std::move( entry ) );
	}

}