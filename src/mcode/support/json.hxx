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

	// Appends `text` escaped as a JSON string body (no surrounding quotes).
	//
	// ONE implementation on purpose. Three hand-rolled copies existed, and the one
	// on the eval-record path escaped only its `detail` field while interpolating
	// the rest raw -- so a task id containing a quote produced a record no parser
	// would read back. Every control character below 0x20 is escaped, not just the
	// five with short forms, because a bare 0x01 is equally invalid in a JSON
	// string.
	auto append_escaped( std::string& out, std::string_view text ) -> void;

	// A JSON value being built. Recursive because a request body is nested: an
	// array of messages, each an object with an array of content blocks.
	//
	// Members are held in a map, so an object serializes in sorted key order
	// regardless of the order it was built in. That is a contract, not a side
	// effect: the prompt cache hashes the serialized prefix, so two runs that set
	// the same fields must produce the same bytes. Array order is the caller's and
	// is preserved.
	struct node {
		enum class kind { null_value, boolean, integer, real, string, array, object };

		kind type = kind::null_value;
		bool boolean = false;
		std::int64_t integer = 0;
		double real = 0.0;
		std::string text;

		std::vector< node > items;
		std::map< std::string, node, std::less<> > members;

		// Factories rather than designated initializers: `node{ .type = ... }` leaves
		// the other members unspecified and every toolchain warns about it, so the
		// constructor form is what callers use.
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
		// Renders whatever the pointer addresses to text. Use the typed variants
		// below when the type matters: a JSON `false` renders as "false", and a
		// numeric field that arrives as a string would otherwise be invisible.
		[[nodiscard]] auto pointer( std::string_view path ) const -> result< std::string >;

		// The JSON TEXT at the pointer, verbatim. Unlike `pointer`, a string stays
		// quoted, so the result can be re-embedded in a document without corruption.
		// Replay needs this: a payload read back and re-emitted must be byte-equal.
		[[nodiscard]] auto pointer_raw( std::string_view path ) const -> result< std::string >;
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

		// The member names of the object at `path`; an empty path is the root. Empty
		// for a document that is not an object, or a pointer that resolves to a
		// non-object. Used to reject unknown keys: a typo in a descriptor is
		// otherwise dropped silently and surfaces as a wrong value much later.
		[[nodiscard]] auto keys_at( std::string_view path ) const -> std::vector< std::string >;

		auto set_string( std::string_view key, std::string_view text ) -> status;
		auto set_int( std::string_view key, std::int64_t number ) -> status;
		auto set_bool( std::string_view key, bool value ) -> status;
		auto set_real( std::string_view key, double value ) -> status;

		// Attaches a whole subtree. The node is moved in, so a caller builds a
		// message object and appends it to an array without a copy per element.
		auto set_node( std::string_view key, node value ) -> status;
		auto append( node value ) -> status;

		// Builds an empty object or array at `key`, returning a pointer into this
		// document that stays valid until the document is destroyed or re-set. Null
		// on a non-mutable document, an empty key, or a key already holding a
		// non-container.
		[[nodiscard]] auto make_object_at( std::string_view key ) -> node*;
		[[nodiscard]] auto make_array_at( std::string_view key ) -> node*;

		// Parses `text` and attaches the result at `key`. This is how a
		// pre-rendered schema or a raw argument blob is embedded without the caller
		// re-encoding it field by field -- and it is the only path that preserves
		// the source's own key order.
		auto set_json( std::string_view key, std::string_view text ) -> status;

		[[nodiscard]] auto dump( bool pretty = false ) const -> result< std::string >;
		[[nodiscard]] auto size( ) const noexcept -> std::size_t;

	private:
		explicit document( yyjson_doc* doc ) : doc_( doc ) { }
		explicit document( mut_doc_pointer doc );

		yyjson_doc* doc_ = nullptr;
		mut_doc_pointer mut_{ nullptr, nullptr };

		// The mutable document's whole content. One representation rather than a
		// flat map plus a tree, so nesting cannot drift from the flat path. Its type
		// is set to `object` by the constructor that makes a document mutable.
		node root_ = node{ };
		bool mutable_ = false;
	};

}
