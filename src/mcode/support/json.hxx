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

	// appends `text` escaped as a JSON string body, escaping every control character.
	auto append_escaped( std::string& out, std::string_view text ) -> void;

	// object members serialize in sorted key order, because the prompt cache hashes the prefix.
	struct node {
		enum class kind { null_value, boolean, integer, real, string, array, object };

		kind type = kind::null_value;
		bool boolean = false;
		std::int64_t integer = 0;
		double real = 0.0;
		std::string text;

		std::vector< node > items;
		std::map< std::string, node, std::less<> > members;

		[[nodiscard]] static auto make_boolean( bool value ) -> node;
		[[nodiscard]] static auto make_integer( std::int64_t value ) -> node;
		[[nodiscard]] static auto make_real( double value ) -> node;
		[[nodiscard]] static auto make_string( std::string_view value ) -> node;
		[[nodiscard]] static auto make_array( ) -> node;
		[[nodiscard]] static auto make_object( ) -> node;

		[[nodiscard]] auto member( std::string_view key ) -> node*;
		[[nodiscard]] auto member( std::string_view key ) const -> const node*;
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
		// a JSON false renders as "false"; prefer the typed variants when the type matters.
		[[nodiscard]] auto pointer( std::string_view path ) const -> result< std::string >;

		// verbatim JSON text: a string stays quoted, so a payload re-emits byte-equal.
		[[nodiscard]] auto pointer_raw( std::string_view path ) const -> result< std::string >;
		[[nodiscard]] auto pointer_string( std::string_view path ) const -> result< std::string >;
		[[nodiscard]] auto pointer_int( std::string_view path ) const -> result< std::int64_t >;
		[[nodiscard]] auto pointer_bool( std::string_view path ) const -> result< bool >;

		// a non-array, or a non-string element, is an error rather than a filtered list.
		[[nodiscard]] auto pointer_string_array( std::string_view path ) const
			-> result< std::vector< std::string > >;

		[[nodiscard]] auto has_pointer( std::string_view path ) const noexcept -> bool;

		[[nodiscard]] auto keys_at( std::string_view path ) const -> std::vector< std::string >;

		auto set_string( std::string_view key, std::string_view text ) -> status;
		auto set_int( std::string_view key, std::int64_t number ) -> status;
		auto set_bool( std::string_view key, bool value ) -> status;
		auto set_real( std::string_view key, double value ) -> status;

		auto set_node( std::string_view key, node value ) -> status;
		auto append( node value ) -> status;

		// the returned pointer stays valid until the document is destroyed or re-set.
		[[nodiscard]] auto make_object_at( std::string_view key ) -> node*;
		[[nodiscard]] auto make_array_at( std::string_view key ) -> node*;

		auto set_json( std::string_view key, std::string_view text ) -> status;

		[[nodiscard]] auto dump( bool pretty = false ) const -> result< std::string >;
		[[nodiscard]] auto size( ) const noexcept -> std::size_t;

		// the read side of `set_json`: a subtree read back must re-embed byte-equal.
		[[nodiscard]] auto root_node( ) const -> result< node >;

	private:
		explicit document( yyjson_doc* doc ) : doc_( doc ) { }
		explicit document( mut_doc_pointer doc );

		yyjson_doc* doc_ = nullptr;
		mut_doc_pointer mut_{ nullptr, nullptr };

		node root_ = node{ };
		bool mutable_ = false;
	};

	[[nodiscard]] auto node_from_json( std::string_view text ) -> result< node >;

	[[nodiscard]] auto node_at( const document& source, std::string_view path ) -> result< node >;

	// sorted keys: thrash detection hashes arguments, and key order is not a difference.
	[[nodiscard]] auto canonicalize( std::string_view text ) -> result< std::string >;

}
