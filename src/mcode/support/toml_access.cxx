#include "mcode/support/toml.hxx"

#include <string>
#include <vector>

namespace mcode::toml {

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

	auto table::optional_string( const std::string_view key ) const
		-> result< std::optional< std::string > > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::optional< std::string >{ };
		}

		if ( auto text = found->as_string( ) ) {
			return std::optional< std::string >{ *text };
		}

		// Present but the wrong type. Reporting it as absent would drop a setting
		// the user wrote.
		return std::unexpected( fail( errc::config,
			"'" + std::string{ key } + "' is not a string" ) );
	}

	auto table::optional_int( const std::string_view key ) const
		-> result< std::optional< std::int64_t > > {
		const auto* found = find( key );

		if ( found == nullptr ) {
			return std::optional< std::int64_t >{ };
		}

		if ( auto number = found->as_int( ) ) {
			return std::optional< std::int64_t >{ *number };
		}

		return std::unexpected( fail( errc::config,
			"'" + std::string{ key } + "' is not an integer" ) );
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

}
