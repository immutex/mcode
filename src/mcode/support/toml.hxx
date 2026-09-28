#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::toml {

	// A minimal TOML reader covering exactly the subset mcode's config and
	// manifests use: top-level keys, `[table]` sections, dotted keys, strings
	// (basic and literal), integers, floats, booleans, and arrays of those.
	//
	// Deliberately not a full TOML 1.0 implementation. No TOML dependency was
	// TOML dependency, and the alternative -- adding one -- costs a third-party
	// library for a format we read and never write. What is unsupported is
	// REFUSED rather than ignored: a date, an inline table, or a multi-line
	// string is a parse error, not a silently dropped key. That is the same rule
	// as unknown manifest keys (`19`).
	//
	// A `[table]` and a dotted key are equivalent: `[a.b]` and `a.b = ...`
	// produce the same flattened key, which is what makes lookup uniform.

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
		[[nodiscard]] auto as_bool( ) const -> result< bool >;
		[[nodiscard]] auto as_string_array( ) const -> result< std::vector< std::string > >;
	};

	class table {
	public:
		// Flattened: a key is the full dotted path, e.g. "permissions" or
		// "stream.usage.in".
		[[nodiscard]] auto find( std::string_view key ) const -> const value*;
		[[nodiscard]] auto contains( std::string_view key ) const noexcept -> bool;

		[[nodiscard]] auto get_string( std::string_view key ) const -> result< std::string >;
		[[nodiscard]] auto get_int( std::string_view key ) const -> result< std::int64_t >;
		[[nodiscard]] auto get_bool( std::string_view key ) const -> result< bool >;
		[[nodiscard]] auto get_string_array( std::string_view key ) const
			-> result< std::vector< std::string > >;

		// Optional variants: absence is not an error, but a WRONG TYPE is.
		//
		// Returning a bare optional made `description = 42` indistinguishable from
		// no description at all, so a mistyped setting was silently ignored -- the
		// failure mode the parser exists to prevent. The outer expected carries the
		// type error; the inner optional carries absence.
		[[nodiscard]] auto optional_string( std::string_view key ) const
			-> result< std::optional< std::string > >;
		[[nodiscard]] auto optional_int( std::string_view key ) const
			-> result< std::optional< std::int64_t > >;

		[[nodiscard]] auto keys( ) const noexcept -> const std::map< std::string, value, std::less<> >& {
			return values_;
		}

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return values_.size( ); }

		// Rejects any key not in `allowed`, with a dotted prefix. This is the
		// mechanism behind the manifest's unknown-key rule (`19`).
		auto reject_unknown( const std::vector< std::string_view >& allowed ) const -> status;

	private:
		friend auto parse( std::string_view text ) -> result< table >;

		std::map< std::string, value, std::less<> > values_;
	};

	// Parses TOML text. The error message names the line number.
	[[nodiscard]] auto parse( std::string_view text ) -> result< table >;

}
