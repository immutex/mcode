#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::toml {

	// a TOML subset; unsupported forms (dates, inline tables) are refused, not dropped.

	enum class value_kind {
		string,
		integer,
		floating,
		boolean,
		array,
	};

	struct value {
		value_kind kind = value_kind::string;

		std::string text;
		std::int64_t integer = 0;
		double floating = 0.0;
		bool boolean = false;
		std::vector< value > items;

		[[nodiscard]] auto as_string( ) const -> result< std::string >;
		[[nodiscard]] auto as_int( ) const -> result< std::int64_t >;

		[[nodiscard]] auto as_double( ) const -> result< double >;
		[[nodiscard]] auto as_bool( ) const -> result< bool >;
		[[nodiscard]] auto as_string_array( ) const -> result< std::vector< std::string > >;
	};

	class table {
	public:
		[[nodiscard]] auto find( std::string_view key ) const -> const value*;
		[[nodiscard]] auto contains( std::string_view key ) const noexcept -> bool;

		[[nodiscard]] auto get_string( std::string_view key ) const -> result< std::string >;
		[[nodiscard]] auto get_int( std::string_view key ) const -> result< std::int64_t >;
		[[nodiscard]] auto get_bool( std::string_view key ) const -> result< bool >;
		[[nodiscard]] auto get_string_array( std::string_view key ) const
			-> result< std::vector< std::string > >;

		// absence is not an error, but a wrong type is.
		[[nodiscard]] auto optional_string( std::string_view key ) const
			-> result< std::optional< std::string > >;
		[[nodiscard]] auto optional_int( std::string_view key ) const
			-> result< std::optional< std::int64_t > >;

		[[nodiscard]] auto keys( ) const noexcept -> const std::map< std::string, value, std::less<> >& {
			return values_;
		}

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return values_.size( ); }

		auto reject_unknown( const std::vector< std::string_view >& allowed ) const -> status;

	private:
		friend auto parse( std::string_view text ) -> result< table >;

		std::map< std::string, value, std::less<> > values_;
	};

	[[nodiscard]] auto parse( std::string_view text ) -> result< table >;

}
