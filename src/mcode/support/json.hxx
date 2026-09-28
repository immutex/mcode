#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

struct yyjson_doc;
struct yyjson_mut_doc;

namespace mcode::json {

	struct value {
		enum class kind { string, integer };

		kind type = kind::string;
		std::string text;
		std::int64_t number = 0;
	};

	using mut_doc_pointer = std::unique_ptr< yyjson_mut_doc, void ( * )( yyjson_mut_doc* ) >;

	class document {
	public:
		document( ) = default;
		~document( );

		document( document&& other ) noexcept;
		auto operator=( document&& other ) noexcept -> document&;

		document( const document& ) = delete;
		auto operator=( const document& ) -> document& = delete;

		[[nodiscard]] static auto parse( std::string_view text ) -> result< document >;
		[[nodiscard]] static auto make_object( ) -> document;

		[[nodiscard]] auto valid( ) const noexcept -> bool { return doc_ != nullptr || mut_ != nullptr; }
		[[nodiscard]] auto is_mutable( ) const noexcept -> bool { return mutable_; }

		[[nodiscard]] auto get_int( std::string_view key ) const -> result< std::int64_t >;
		[[nodiscard]] auto get_string( std::string_view key ) const -> result< std::string >;
		// Renders whatever the pointer addresses to text. Use the typed variants
		// below when the type matters: a JSON `false` renders as "false", and a
		// numeric field that arrives as a string would otherwise be invisible.
		[[nodiscard]] auto pointer( std::string_view path ) const -> result< std::string >;
		[[nodiscard]] auto pointer_string( std::string_view path ) const -> result< std::string >;
		[[nodiscard]] auto pointer_int( std::string_view path ) const -> result< std::int64_t >;
		[[nodiscard]] auto pointer_bool( std::string_view path ) const -> result< bool >;

		// Reads an array of strings. A non-array, or an array containing a
		// non-string, is an error rather than a silently filtered list -- a
		// descriptor with one bad entry should not load with the rest.
		[[nodiscard]] auto pointer_string_array( std::string_view path ) const
			-> result< std::vector< std::string > >;

		// True when the pointer resolves to anything at all. A missing pointer is
		// the normal case for a streaming delta -- most payloads carry one field --
		// so this is a query, not an error.
		[[nodiscard]] auto has_pointer( std::string_view path ) const noexcept -> bool;

		auto set_string( std::string_view key, std::string_view text ) -> status;
		auto set_int( std::string_view key, std::int64_t number ) -> status;

		[[nodiscard]] auto dump( bool pretty = false ) const -> result< std::string >;
		[[nodiscard]] auto size( ) const noexcept -> std::size_t;

	private:
		explicit document( yyjson_doc* doc ) : doc_( doc ) { }
		explicit document( mut_doc_pointer doc );

		yyjson_doc* doc_ = nullptr;
		mut_doc_pointer mut_{ nullptr, nullptr };
		std::map< std::string, value, std::less<> > members_;
		bool mutable_ = false;
	};

}
