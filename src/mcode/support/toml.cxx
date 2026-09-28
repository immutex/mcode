#include "mcode/support/toml.hxx"

#include <cctype>
#include <charconv>
#include <optional>

namespace mcode::toml {

	namespace {

		auto is_space( const char character ) -> bool {
			return character == ' ' || character == '\t' || character == '\r';
		}

		auto is_bare_key_char( const char character ) -> bool {
			return std::isalnum( static_cast< unsigned char >( character ) ) != 0 ||
				character == '_' || character == '-';
		}

		struct reader {
			std::string_view text;
			std::size_t position = 0;
			std::size_t line = 1;

			[[nodiscard]] auto at_end( ) const noexcept -> bool { return position >= text.size( ); }
			[[nodiscard]] auto peek( ) const noexcept -> char {
				return at_end( ) ? '\0' : text[ position ];
			}

			auto advance( ) -> char {
				const auto character = peek( );
				++position;

				if ( character == '\n' ) {
					++line;
				}

				return character;
			}

			auto skip_spaces( ) -> void {
				while ( !at_end( ) && is_space( peek( ) ) ) {
					advance( );
				}
			}

			auto skip_to_newline( ) -> void {
				while ( !at_end( ) && peek( ) != '\n' ) {
					advance( );
				}
			}

			[[nodiscard]] auto failure( const std::string_view message ) const -> error {
				return fail( errc::config, "toml line " + std::to_string( line ) + ": " +
					std::string{ message } );
			}
		};

		auto parse_quoted( reader& input, const char quote ) -> result< std::string > {
			auto out = std::string{ };

			while ( true ) {
				if ( input.at_end( ) || input.peek( ) == '\n' ) {
					return std::unexpected( input.failure( "unterminated string" ) );
				}

				const auto character = input.advance( );

				if ( character == quote ) {
					return out;
				}

				// Only the escapes that appear in real config files. An unsupported
				// escape is refused rather than passed through, because a silently
				// literal backslash changes a value's meaning.
				if ( character == '\\' && quote == '"' ) {
					if ( input.at_end( ) ) {
						return std::unexpected( input.failure( "unterminated escape" ) );
					}

					const auto escaped = input.advance( );

					switch ( escaped ) {
						case 'n': out.push_back( '\n' ); break;
						case 't': out.push_back( '\t' ); break;
						case 'r': out.push_back( '\r' ); break;
						case '"': out.push_back( '"' ); break;
						case '\\': out.push_back( '\\' ); break;
						default:
							return std::unexpected( input.failure(
								std::string{ "unsupported escape \\" } + escaped ) );
					}

					continue;
				}

				out.push_back( character );
			}
		}

		auto parse_bare_key( reader& input ) -> result< std::string > {
			auto out = std::string{ };

			while ( !input.at_end( ) && is_bare_key_char( input.peek( ) ) ) {
				out.push_back( input.advance( ) );
			}

			if ( out.empty( ) ) {
				return std::unexpected( input.failure( "expected a key" ) );
			}

			return out;
		}

		// A key may be dotted (`a.b = 1`), which flattens to "a.b".
		auto parse_key( reader& input ) -> result< std::string > {
			auto out = std::string{ };

			while ( true ) {
				input.skip_spaces( );

				auto part = std::string{ };

				if ( input.peek( ) == '"' || input.peek( ) == '\'' ) {
					const auto quote = input.advance( );
					auto quoted = parse_quoted( input, quote );

					if ( !quoted ) {
						return std::unexpected( quoted.error( ) );
					}

					part = *quoted;
				} else {
					auto bare = parse_bare_key( input );

					if ( !bare ) {
						return std::unexpected( bare.error( ) );
					}

					part = *bare;
				}

				out += part;
				input.skip_spaces( );

				if ( input.peek( ) != '.' ) {
					return out;
				}

				input.advance( );
				out.push_back( '.' );
			}
		}

		auto parse_value( reader& input ) -> result< value >;

		auto parse_array( reader& input ) -> result< value > {
			auto out = value{ };
			out.kind = value_kind::array;

			input.advance( );

			while ( true ) {
				input.skip_spaces( );

				// Arrays may span lines.
				while ( !input.at_end( ) && input.peek( ) == '\n' ) {
					input.advance( );
					input.skip_spaces( );
				}

				if ( input.at_end( ) ) {
					return std::unexpected( input.failure( "unterminated array" ) );
				}

				if ( input.peek( ) == ']' ) {
					input.advance( );

					return out;
				}

				auto item = parse_value( input );

				if ( !item ) {
					return std::unexpected( item.error( ) );
				}

				// Homogeneity is required by TOML and is what lets a caller ask for a
				// string array without checking each element's type at the call site.
				if ( !out.items.empty( ) && out.items.front( ).kind != item->kind ) {
					return std::unexpected( input.failure( "array elements must share a type" ) );
				}

				out.items.push_back( std::move( *item ) );

				input.skip_spaces( );

				if ( input.peek( ) == ',' ) {
					input.advance( );

					continue;
				}

				if ( input.peek( ) == ']' ) {
					input.advance( );

					return out;
				}

				return std::unexpected( input.failure( "expected ',' or ']' in array" ) );
			}
		}

		auto parse_number( reader& input ) -> result< value > {
			auto out = value{ };

			const auto start = input.position;
			auto is_float = false;

			if ( input.peek( ) == '-' || input.peek( ) == '+' ) {
				input.advance( );
			}

			while ( !input.at_end( ) ) {
				const auto character = input.peek( );

				if ( std::isdigit( static_cast< unsigned char >( character ) ) != 0 ||
					character == '_' ) {
					input.advance( );

					continue;
				}

				if ( character == '.' || character == 'e' || character == 'E' ) {
					is_float = true;
					input.advance( );

					continue;
				}

				break;
			}

			auto literal = std::string{ input.text.substr( start, input.position - start ) };

			// TOML allows underscores as digit separators.
			auto cleaned = std::string{ };

			for ( const auto character : literal ) {
				if ( character != '_' ) {
					cleaned.push_back( character );
				}
			}

			if ( cleaned.empty( ) || cleaned == "-" || cleaned == "+" ) {
				return std::unexpected( input.failure( "expected a value" ) );
			}

			if ( is_float ) {
				out.kind = value_kind::floating;

				try {
					out.floating = std::stod( cleaned );
				} catch ( ... ) {
					return std::unexpected( input.failure( "malformed number: " + cleaned ) );
				}

				return out;
			}

			out.kind = value_kind::integer;

			const auto* first = cleaned.data( );
			const auto* last = cleaned.data( ) + cleaned.size( );
			const auto parsed = std::from_chars( first, last, out.integer );

			if ( parsed.ec != std::errc{ } || parsed.ptr != last ) {
				return std::unexpected( input.failure( "malformed integer: " + cleaned ) );
			}

			return out;
		}

		auto parse_value( reader& input ) -> result< value > {
			input.skip_spaces( );

			if ( input.at_end( ) ) {
				return std::unexpected( input.failure( "expected a value" ) );
			}

			const auto character = input.peek( );

			if ( character == '"' || character == '\'' ) {
				input.advance( );

				auto text = parse_quoted( input, character );

				if ( !text ) {
					return std::unexpected( text.error( ) );
				}

				auto out = value{ };
				out.kind = value_kind::string;
				out.text = std::move( *text );

				return out;
			}

			if ( character == '[' ) {
				return parse_array( input );
			}

			if ( character == '{' ) {
				return std::unexpected( input.failure(
					"inline tables are not supported; use a [table] section" ) );
			}

			// Bare words: true, false, and anything else we do not accept.
			if ( std::isalpha( static_cast< unsigned char >( character ) ) != 0 ) {
				auto word = std::string{ };

				while ( !input.at_end( ) && is_bare_key_char( input.peek( ) ) ) {
					word.push_back( input.advance( ) );
				}

				if ( word == "true" || word == "false" ) {
					auto out = value{ };
					out.kind = value_kind::boolean;
					out.boolean = ( word == "true" );

					return out;
				}

				return std::unexpected( input.failure(
					"unsupported value '" + word + "' (dates and times are not supported)" ) );
			}

			return parse_number( input );
		}

	}

	auto value::as_string( ) const -> result< std::string > {
		if ( kind != value_kind::string ) {
			return std::unexpected( fail( errc::config, "value is not a string" ) );
		}

		return text;
	}

	auto value::as_int( ) const -> result< std::int64_t > {
		if ( kind != value_kind::integer ) {
			return std::unexpected( fail( errc::config, "value is not an integer" ) );
		}

		return integer;
	}

	auto value::as_bool( ) const -> result< bool > {
		if ( kind != value_kind::boolean ) {
			return std::unexpected( fail( errc::config, "value is not a boolean" ) );
		}

		return boolean;
	}

	auto value::as_string_array( ) const -> result< std::vector< std::string > > {
		if ( kind != value_kind::array ) {
			return std::unexpected( fail( errc::config, "value is not an array" ) );
		}

		auto out = std::vector< std::string >{ };

		for ( const auto& item : items ) {
			auto element = item.as_string( );

			if ( !element ) {
				return std::unexpected( element.error( ) );
			}

			out.push_back( std::move( *element ) );
		}

		return out;
	}

	auto table::find( const std::string_view key ) const -> const value* {
		const auto found = values_.find( std::string{ key } );

		return found != values_.end( ) ? &found->second : nullptr;
	}

	auto table::contains( const std::string_view key ) const noexcept -> bool {
		return values_.find( std::string{ key } ) != values_.end( );
	}

	auto table::get_string( const std::string_view key ) const -> result< std::string > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::config, "missing key: " + std::string{ key } ) );
		}

		return found->as_string( );
	}

	auto table::get_int( const std::string_view key ) const -> result< std::int64_t > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::config, "missing key: " + std::string{ key } ) );
		}

		return found->as_int( );
	}

	auto table::get_bool( const std::string_view key ) const -> result< bool > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::config, "missing key: " + std::string{ key } ) );
		}

		return found->as_bool( );
	}

	auto table::get_string_array( const std::string_view key ) const
		-> result< std::vector< std::string > > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::config, "missing key: " + std::string{ key } ) );
		}

		return found->as_string_array( );
	}

	auto table::optional_string( const std::string_view key ) const -> std::optional< std::string > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::nullopt;
		}

		if ( auto text = found->as_string( ) ) {
			return *text;
		}

		return std::nullopt;
	}

	auto table::optional_int( const std::string_view key ) const -> std::optional< std::int64_t > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::nullopt;
		}

		if ( auto number = found->as_int( ) ) {
			return *number;
		}

		return std::nullopt;
	}

	auto table::reject_unknown( const std::vector< std::string_view >& allowed ) const -> status {
		for ( const auto& [ key, entry ] : values_ ) {
			(void)entry;

			// A table section such as `stream.usage.in` is allowed if its parent is
			// allowed: the caller lists the tables it understands, not every leaf.
			auto known = false;

			for ( const auto& candidate : allowed ) {
				if ( key == candidate ||
					key.starts_with( std::string{ candidate } + "." ) ) {
					known = true;

					break;
				}
			}

			if ( !known ) {
				return std::unexpected( fail( errc::config, "unknown key: " + key ) );
			}
		}

		return { };
	}

	auto parse( const std::string_view text ) -> result< table > {
		auto out = table{ };
		auto input = reader{ };
		input.text = text;
		auto prefix = std::string{ };

		while ( !input.at_end( ) ) {
			input.skip_spaces( );

			if ( input.at_end( ) ) {
				break;
			}

			if ( input.peek( ) == '\n' ) {
				input.advance( );

				continue;
			}

			if ( input.peek( ) == '#' ) {
				input.skip_to_newline( );

				continue;
			}

			// A table header sets the prefix for every key that follows.
			if ( input.peek( ) == '[' ) {
				input.advance( );

				auto name = parse_key( input );

				if ( !name ) {
					return std::unexpected( name.error( ) );
				}

				input.skip_spaces( );

				if ( input.peek( ) != ']' ) {
					return std::unexpected( input.failure( "expected ']' after table name" ) );
				}

				input.advance( );

				input.skip_spaces( );

				if ( !input.at_end( ) && input.peek( ) != '\n' && input.peek( ) != '#' ) {
					return std::unexpected( input.failure( "unexpected text after table header" ) );
				}

				prefix = *name + ".";
				input.skip_to_newline( );

				continue;
			}

			auto key = parse_key( input );

			if ( !key ) {
				return std::unexpected( key.error( ) );
			}

			input.skip_spaces( );

			if ( input.peek( ) != '=' ) {
				return std::unexpected( input.failure( "expected '=' after key '" + *key + "'" ) );
			}

			input.advance( );

			auto parsed = parse_value( input );

			if ( !parsed ) {
				return std::unexpected( parsed.error( ) );
			}

			const auto full_key = prefix + *key;

			if ( out.values_.contains( full_key ) ) {
				return std::unexpected( input.failure( "duplicate key: " + full_key ) );
			}

			out.values_.emplace( full_key, std::move( *parsed ) );

			input.skip_spaces( );

			if ( !input.at_end( ) && input.peek( ) != '\n' && input.peek( ) != '#' ) {
				return std::unexpected( input.failure(
					"unexpected text after value for key '" + full_key + "'" ) );
			}

			input.skip_to_newline( );
		}

		return out;
	}

}
