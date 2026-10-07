#include "mcode/tools/json_repair.hxx"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::tools {

	namespace {

		inline constexpr std::string_view FENCE = "```";
		inline constexpr char QUOTE_DOUBLE = '"';
		inline constexpr char QUOTE_SINGLE = '\'';

		[[nodiscard]] auto is_space( const char character ) noexcept -> bool {
			return character == ' ' || character == '\t' || character == '\n' ||
				character == '\r';
		}

		[[nodiscard]] auto is_open_bracket( const char character ) noexcept -> bool {
			return character == '{' || character == '[';
		}

		[[nodiscard]] auto is_close_bracket( const char character ) noexcept -> bool {
			return character == '}' || character == ']';
		}

		[[nodiscard]] auto closer_for( const char opener ) noexcept -> char {
			return opener == '{' ? '}' : ']';
		}

		[[nodiscard]] auto parses( const std::string_view text ) -> bool {
			return json::document::parse( text ).has_value( );
		}

		// `,` with nothing but whitespace before the closing bracket is the commonest break.
		[[nodiscard]] auto drop_trailing_commas( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			auto quote = char{ 0 };
			auto escaped = false;

			for ( auto index = std::size_t{ 0 }; index < text.size( ); ++index ) {
				const auto character = text[ index ];

				if ( quote != 0 ) {
					out += character;

					if ( escaped ) {
						escaped = false;
					} else if ( character == '\\' ) {
						escaped = true;
					} else if ( character == quote ) {
						quote = 0;
					}

					continue;
				}

				if ( character == QUOTE_DOUBLE ) {
					quote = character;
					out += character;

					continue;
				}

				if ( character == ',' ) {
					auto next = index + 1;

					while ( next < text.size( ) && is_space( text[ next ] ) ) {
						++next;
					}

					if ( next < text.size( ) && is_close_bracket( text[ next ] ) ) {
						continue;
					}
				}

				out += character;
			}

			return out;
		}

		// Python literals only occur outside a string, so the scan tracks quote state.
		[[nodiscard]] auto swap_python_literals( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			auto quote = char{ 0 };
			auto escaped = false;
			auto index = std::size_t{ 0 };

			while ( index < text.size( ) ) {
				const auto character = text[ index ];

				if ( quote != 0 ) {
					out += character;

					if ( escaped ) {
						escaped = false;
					} else if ( character == '\\' ) {
						escaped = true;
					} else if ( character == quote ) {
						quote = 0;
					}

					++index;

					continue;
				}

				if ( character == QUOTE_DOUBLE || character == QUOTE_SINGLE ) {
					quote = character;
					out += character;
					++index;

					continue;
				}

				const auto rest = text.substr( index );

				if ( rest.starts_with( "True" ) ) {
					out += "true";
					index += 4;

					continue;
				}

				if ( rest.starts_with( "False" ) ) {
					out += "false";
					index += 5;

					continue;
				}

				if ( rest.starts_with( "None" ) ) {
					out += "null";
					index += 4;

					continue;
				}

				out += character;
				++index;
			}

			return out;
		}

		// Single-quoted strings are not JSON. Outside a double-quoted string a `'` delimits one,
		// so it becomes `"` and an embedded `"` is escaped to keep the result well formed.
		[[nodiscard]] auto swap_single_quotes( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			auto in_double = false;
			auto in_single = false;
			auto index = std::size_t{ 0 };

			while ( index < text.size( ) ) {
				const auto character = text[ index ];

				if ( in_double ) {
					out += character;

					if ( character == '\\' && index + 1 < text.size( ) ) {
						out += text[ index + 1 ];
						index += 2;

						continue;
					}

					if ( character == QUOTE_DOUBLE ) {
						in_double = false;
					}

					++index;

					continue;
				}

				if ( in_single ) {
					if ( character == '\\' && index + 1 < text.size( ) ) {
						if ( text[ index + 1 ] == QUOTE_SINGLE ) {
							out += QUOTE_SINGLE;
						} else {
							out += character;
							out += text[ index + 1 ];
						}

						index += 2;

						continue;
					}

					if ( character == QUOTE_SINGLE ) {
						out += QUOTE_DOUBLE;
						in_single = false;
						++index;

						continue;
					}

					if ( character == QUOTE_DOUBLE ) {
						out += '\\';
					}

					out += character;
					++index;

					continue;
				}

				if ( character == QUOTE_DOUBLE ) {
					in_double = true;
					out += character;
					++index;

					continue;
				}

				if ( character == QUOTE_SINGLE ) {
					in_single = true;
					out += QUOTE_DOUBLE;
					++index;

					continue;
				}

				out += character;
				++index;
			}

			return out;
		}

		// The end of the first balanced `{...}` or `[...]` starting at `start`, or nothing.
		[[nodiscard]] auto scan_balanced( const std::string_view text, const std::size_t start )
			-> std::optional< std::size_t > {
			auto depth = std::size_t{ 0 };
			auto quote = char{ 0 };
			auto escaped = false;

			for ( auto index = start; index < text.size( ); ++index ) {
				const auto character = text[ index ];

				if ( quote != 0 ) {
					if ( escaped ) {
						escaped = false;
					} else if ( character == '\\' ) {
						escaped = true;
					} else if ( character == quote ) {
						quote = 0;
					}

					continue;
				}

				if ( character == QUOTE_DOUBLE || character == QUOTE_SINGLE ) {
					quote = character;

					continue;
				}

				if ( is_open_bracket( character ) ) {
					++depth;

					continue;
				}

				if ( is_close_bracket( character ) ) {
					if ( depth == 0 ) {
						return std::nullopt;
					}

					--depth;

					if ( depth == 0 ) {
						return index + 1;
					}
				}
			}

			return std::nullopt;
		}

		[[nodiscard]] auto first_open_at( const std::string_view text, const char opener )
			-> std::size_t {
			for ( auto index = std::size_t{ 0 }; index < text.size( ); ++index ) {
				if ( text[ index ] == opener ) {
					return index;
				}
			}

			return std::string_view::npos;
		}

		// Strips a markdown fence and any prose around the value. A fence is the single largest
		// cause of a zero parse rate, so it is removed before anything else is attempted.
		[[nodiscard]] auto extract_value( const std::string_view text ) -> std::string {
			auto body = std::string{ text };

			auto begin = std::size_t{ 0 };

			while ( begin < body.size( ) && is_space( body[ begin ] ) ) {
				++begin;
			}

			if ( body.compare( begin, FENCE.size( ), FENCE ) == 0 ) {
				const auto after_fence = body.find( '\n', begin );

				if ( after_fence == std::string::npos ) {
					body.clear( );
				} else {
					body.erase( 0, after_fence + 1 );

					const auto closing = body.rfind( FENCE );

					if ( closing != std::string::npos ) {
						body.erase( closing );
					}
				}
			}

			// tool arguments are an object, so `{` is preferred over a bracket in prose.
			for ( const auto opener : { '{', '[' } ) {
				const auto start = first_open_at( body, opener );

				if ( start == std::string::npos ) {
					continue;
				}

				if ( const auto end = scan_balanced( body, start ) ) {
					return body.substr( start, *end - start );
				}

				return body.substr( start );
			}

			return body;
		}

		// Closes the brackets left open by a response cut off at a value boundary. A tail that
		// stops mid-value is left alone: completing it would invent content the model never sent.
		[[nodiscard]] auto close_truncated( const std::string_view text ) -> std::string {
			auto stack = std::vector< char >{ };
			auto quote = char{ 0 };
			auto escaped = false;
			auto last_value_end = std::size_t{ 0 };
			auto last_value_stack = std::vector< char >{ };
			auto index = std::size_t{ 0 };

			const auto record = [ & ]( const std::size_t position ) {
				last_value_end = position;
				last_value_stack = stack;
			};

			while ( index < text.size( ) ) {
				const auto character = text[ index ];

				if ( quote != 0 ) {
					if ( escaped ) {
						escaped = false;
					} else if ( character == '\\' ) {
						escaped = true;
					} else if ( character == quote ) {
						quote = 0;
						record( index + 1 );
					}

					++index;

					continue;
				}

				if ( character == QUOTE_DOUBLE || character == QUOTE_SINGLE ) {
					quote = character;
					++index;

					continue;
				}

				if ( is_open_bracket( character ) ) {
					stack.push_back( closer_for( character ) );
					++index;

					continue;
				}

				if ( is_close_bracket( character ) ) {
					if ( stack.empty( ) ) {
						return std::string{ text };
					}

					stack.pop_back( );
					record( index + 1 );
					++index;

					continue;
				}

				if ( is_space( character ) || character == ',' || character == ':' ) {
					++index;

					continue;
				}

				auto end = index;

				while ( end < text.size( ) && !is_space( text[ end ] ) && text[ end ] != ',' &&
					text[ end ] != '}' && text[ end ] != ']' && text[ end ] != ':' ) {
					++end;
				}

				record( end );
				index = end;
			}

			if ( stack.empty( ) || last_value_end == 0 ) {
				return std::string{ text };
			}

			// anything but whitespace after the last complete value means the cut was mid-member.
			for ( auto tail = last_value_end; tail < text.size( ); ++tail ) {
				if ( !is_space( text[ tail ] ) ) {
					return std::string{ text };
				}
			}

			auto out = std::string{ text.substr( 0, last_value_end ) };

			for ( auto closer = last_value_stack.rbegin( ); closer != last_value_stack.rend( );
				++closer ) {
				out += *closer;
			}

			return out;
		}

	}

	auto repair_json( const std::string_view text ) -> result< std::string > {
		if ( text.empty( ) ) {
			return std::unexpected( fail( errc::json, "empty arguments" ) );
		}

		auto candidate = extract_value( text );

		if ( parses( candidate ) ) {
			return candidate;
		}

		candidate = drop_trailing_commas( candidate );

		if ( parses( candidate ) ) {
			return candidate;
		}

		candidate = swap_python_literals( candidate );

		if ( parses( candidate ) ) {
			return candidate;
		}

		candidate = swap_single_quotes( candidate );

		if ( parses( candidate ) ) {
			return candidate;
		}

		const auto closed = close_truncated( candidate );

		if ( closed != candidate && parses( closed ) ) {
			return closed;
		}

		return std::unexpected( fail( errc::json,
			"arguments are not JSON and no repair applies" ) );
	}

}
