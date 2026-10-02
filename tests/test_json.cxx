#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <type_traits>
#include <utility>

#include "mcode/support/json.hxx"

using mcode::json::document;

TEST_CASE( "parsing a valid object exposes typed accessors", "[json]" ) {
	auto doc = document::parse( R"({"model":"mcode","steps":3,"ok":true})" );
	REQUIRE( doc );

	auto model = doc->get_string( "model" );
	REQUIRE( model );
	CHECK( *model == "mcode" );

	auto steps = doc->get_int( "steps" );
	REQUIRE( steps );
	CHECK( *steps == 3 );
}

TEST_CASE( "a missing key is an error, never a defaulted value", "[json]" ) {
	auto doc = document::parse( R"({"present":1})" );
	REQUIRE( doc );

	auto missing = doc->get_int( "absent" );
	REQUIRE_FALSE( missing );
	CHECK( missing.error( ).code == mcode::errc::json );

	auto present = doc->get_int( "present" );
	REQUIRE( present );
	CHECK( *present == 1 );
}

TEST_CASE( "a type mismatch is an error, not a coercion", "[json]" ) {
	auto doc = document::parse( R"({"n":1})" );
	REQUIRE( doc );

	auto as_string = doc->get_string( "n" );
	REQUIRE_FALSE( as_string );
	CHECK( as_string.error( ).code == mcode::errc::json );
}

TEST_CASE( "malformed input reports position information", "[json]" ) {
	auto doc = document::parse( "{not json" );
	REQUIRE_FALSE( doc );
	CHECK( doc.error( ).code == mcode::errc::json );
	CHECK( doc.error( ).msg.find( "byte" ) != std::string::npos );
}

TEST_CASE( "empty input is rejected", "[json]" ) {
	auto doc = document::parse( "" );
	REQUIRE_FALSE( doc );
	CHECK( doc.error( ).code == mcode::errc::json );
}

TEST_CASE( "JSON Pointer resolves nested values", "[json]" ) {
	auto doc = document::parse( R"({"a":{"b":{"c":"deep"}}})" );
	REQUIRE( doc );

	auto deep = doc->pointer( "/a/b/c" );
	REQUIRE( deep );
	CHECK( *deep == "deep" );

	auto absent = doc->pointer( "/a/b/z" );
	REQUIRE_FALSE( absent );
}

TEST_CASE( "pointer renders non-string values as JSON", "[json]" ) {
	auto doc = document::parse( R"({"choices":[{"delta":{"content":"hi"}}]})" );
	REQUIRE( doc );

	auto content = doc->pointer( "/choices/0/delta/content" );
	REQUIRE( content );
	CHECK( *content == "hi" );
}

TEST_CASE( "a mutable document round-trips", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.valid( ) );
	REQUIRE( doc.is_mutable( ) );

	REQUIRE( doc.set_string( "name", "mcode" ) );
	REQUIRE( doc.set_int( "version", 1 ) );

	auto text = doc.dump( );
	REQUIRE( text );
	CHECK( text->find( "\"name\":\"mcode\"" ) != std::string::npos );
	CHECK( text->find( "\"version\":1" ) != std::string::npos );
}

TEST_CASE( "mutation on a parse-only document is refused", "[json]" ) {
	auto doc = document::parse( R"({"a":1})" );
	REQUIRE( doc );
	REQUIRE_FALSE( doc->is_mutable( ) );

	auto refused = doc->set_string( "b", "c" );
	REQUIRE_FALSE( refused );
	CHECK( refused.error( ).code == mcode::errc::json );
}

TEST_CASE( "serialization is deterministic regardless of insertion order", "[json]" ) {
	auto first = document::make_object( );
	REQUIRE( first.set_string( "zebra", "1" ) );
	REQUIRE( first.set_string( "alpha", "2" ) );
	REQUIRE( first.set_string( "middle", "3" ) );

	auto second = document::make_object( );
	REQUIRE( second.set_string( "middle", "3" ) );
	REQUIRE( second.set_string( "alpha", "2" ) );
	REQUIRE( second.set_string( "zebra", "1" ) );

	auto first_text = first.dump( );
	auto second_text = second.dump( );
	REQUIRE( first_text );
	REQUIRE( second_text );
	CHECK( *first_text == *second_text );

	const auto alpha = first_text->find( "alpha" );
	const auto middle = first_text->find( "middle" );
	const auto zebra = first_text->find( "zebra" );
	CHECK( alpha < middle );
	CHECK( middle < zebra );
}

TEST_CASE( "an empty mutable document serializes as an empty object", "[json]" ) {
	auto doc = document::make_object( );
	auto text = doc.dump( );
	REQUIRE( text );
	CHECK( *text == "{}" );
}

TEST_CASE( "the mutable member count is tracked", "[json]" ) {
	auto doc = document::make_object( );
	CHECK( doc.size( ) == 0 );
	REQUIRE( doc.set_string( "a", "1" ) );
	REQUIRE( doc.set_string( "b", "2" ) );
	CHECK( doc.size( ) == 2 );

	REQUIRE( doc.set_string( "a", "3" ) );
	CHECK( doc.size( ) == 2 );
}

TEST_CASE( "setting an empty key is refused", "[json]" ) {
	auto doc = document::make_object( );
	auto refused = doc.set_string( "", "value" );
	REQUIRE_FALSE( refused );
	CHECK( refused.error( ).code == mcode::errc::json );
}

TEST_CASE( "a string value with an embedded NUL survives round-tripping", "[json]" ) {
	auto doc = document::make_object( );
	const auto value = std::string{ "ab\0cd", 5 };
	REQUIRE( doc.set_string( "k", value ) );

	auto text = doc.dump( );
	REQUIRE( text );

	auto reparsed = document::parse( *text );
	REQUIRE( reparsed );

	auto back = reparsed->get_string( "k" );
	REQUIRE( back );
	CHECK( *back == value );
}

TEST_CASE( "documents are movable but not copyable", "[json]" ) {
	STATIC_REQUIRE( std::is_move_constructible_v< document > );
	STATIC_REQUIRE_FALSE( std::is_copy_constructible_v< document > );
	STATIC_REQUIRE_FALSE( std::is_copy_assignable_v< document > );
}

TEST_CASE( "a moved-from parsed document is empty rather than dangling", "[json]" ) {
	auto doc = document::parse( R"({"a":1})" );
	REQUIRE( doc );

	auto moved = std::move( *doc );
	CHECK( moved.valid( ) );
	CHECK_FALSE( doc->valid( ) );
}

TEST_CASE( "a moved-from mutable document is empty rather than dangling", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_string( "a", "1" ) );

	auto moved = std::move( doc );
	CHECK( moved.valid( ) );
	CHECK( moved.is_mutable( ) );
	CHECK_FALSE( doc.valid( ) );
}

TEST_CASE( "pretty printing only changes whitespace", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_string( "a", "1" ) );

	auto compact = doc.dump( false );
	auto pretty = doc.dump( true );
	REQUIRE( compact );
	REQUIRE( pretty );

	CHECK( pretty->size( ) > compact->size( ) );
	CHECK( compact->find( '\n' ) == std::string::npos );
	CHECK( pretty->find( '\n' ) != std::string::npos );

	auto reparsed = document::parse( *pretty );
	REQUIRE( reparsed );

	auto value = reparsed->get_string( "a" );
	REQUIRE( value );
	CHECK( *value == "1" );
}

TEST_CASE( "typed pointer accessors read the right types", "[json]" ) {
	auto doc = mcode::json::document::parse(
		R"({"flag":true,"count":42,"name":"x","items":["a","b"]})" );
	REQUIRE( static_cast< bool >( doc ) );

	auto flag = doc->pointer_bool( "/flag" );
	REQUIRE( static_cast< bool >( flag ) );
	REQUIRE( *flag == true );

	REQUIRE_FALSE( static_cast< bool >( doc->pointer_int( "/flag" ) ) );
	REQUIRE_FALSE( static_cast< bool >( doc->pointer_bool( "/count" ) ) );
	REQUIRE_FALSE( static_cast< bool >( doc->pointer_bool( "/missing" ) ) );

	auto count = doc->pointer_int( "/count" );
	REQUIRE( static_cast< bool >( count ) );
	REQUIRE( *count == 42 );

	auto items = doc->pointer_string_array( "/items" );
	REQUIRE( static_cast< bool >( items ) );
	REQUIRE( items->size( ) == 2 );
	REQUIRE( ( *items )[ 0 ] == "a" );

	REQUIRE_FALSE( static_cast< bool >( doc->pointer_string_array( "/name" ) ) );
	REQUIRE_FALSE( static_cast< bool >( doc->pointer_string_array( "/missing" ) ) );
}

TEST_CASE( "a nested body round-trips through the mutable writer", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_string( "model", "gpt-5" ) );

	auto* messages = doc.make_array_at( "messages" );
	REQUIRE( messages != nullptr );

	auto first = mcode::json::node::make_object( );
	REQUIRE( first.members.emplace( "role", mcode::json::node::make_string( "user" ) ).second );

	auto* blocks = &first.members.emplace( "content", mcode::json::node::make_array( ) ).first->second;
	blocks->items.push_back( mcode::json::node::make_object( ) );

	messages->items.push_back( std::move( first ) );

	auto text = doc.dump( );
	REQUIRE( static_cast< bool >( text ) );

	auto reparsed = document::parse( *text );
	REQUIRE( static_cast< bool >( reparsed ) );

	if ( !reparsed ) {
		FAIL( reparsed.error( ).msg );
	}

	auto role = reparsed->pointer_string( "/messages/0/role" );
	REQUIRE( static_cast< bool >( role ) );
	CHECK( *role == "user" );

	CHECK( reparsed->has_pointer( "/messages/0/content/0" ) );
}

TEST_CASE( "nesting preserves the sorted-key contract", "[json]" ) {
	// keys are sorted so equal documents serialize identically, which the prompt cache depends on
	auto build = []( const bool reverse ) {
		auto doc = document::make_object( );

		auto* outer = doc.make_object_at( "options" );
		REQUIRE( outer != nullptr );

		const auto keys = std::array< std::string, 3 >{ "zebra", "alpha", "middle" };

		for ( auto index = std::size_t{ 0 }; index < keys.size( ); ++index ) {
			const auto at = reverse ? ( keys.size( ) - 1 - index ) : index;

			outer->members.emplace( keys[ at ], mcode::json::node::make_integer( static_cast< std::int64_t >( at ) ) );
		}

		return doc.dump( );
	};

	auto forward = build( false );
	auto backward = build( true );

	REQUIRE( static_cast< bool >( forward ) );
	REQUIRE( static_cast< bool >( backward ) );
	CHECK( *forward == *backward );

	const auto alpha = forward->find( "alpha" );
	const auto middle = forward->find( "middle" );
	const auto zebra = forward->find( "zebra" );
	CHECK( alpha < middle );
	CHECK( middle < zebra );
}

TEST_CASE( "set_json embeds pre-rendered JSON as structure", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_json( "schema", R"({"type":"object","properties":{"path":{"type":"string"}}})" ) );

	auto text = doc.dump( );
	REQUIRE( static_cast< bool >( text ) );

	auto reparsed = document::parse( *text );
	REQUIRE( static_cast< bool >( reparsed ) );

	auto kind = reparsed->pointer_string( "/schema/type" );
	REQUIRE( static_cast< bool >( kind ) );
	CHECK( *kind == "object" );

	REQUIRE_FALSE( doc.set_json( "bad", "{not json}" ) );
}

TEST_CASE( "scalar setters round-trip their types", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_bool( "flag", true ) );
	REQUIRE( doc.set_real( "ratio", 0.5 ) );
	REQUIRE( doc.set_int( "count", -3 ) );

	auto text = doc.dump( );
	REQUIRE( static_cast< bool >( text ) );

	auto reparsed = document::parse( *text );
	REQUIRE( static_cast< bool >( reparsed ) );

	auto ratio = reparsed->pointer( "/ratio" );
	REQUIRE( static_cast< bool >( ratio ) );
	CHECK( ratio->find( "0.5" ) != std::string::npos );

	auto flag = reparsed->pointer_bool( "/flag" );
	REQUIRE( static_cast< bool >( flag ) );
	CHECK( *flag );

	auto count = reparsed->pointer_int( "/count" );
	REQUIRE( static_cast< bool >( count ) );
	CHECK( *count == -3 );
}

TEST_CASE( "make_array_at does not silently replace a scalar", "[json]" ) {
	auto doc = document::make_object( );
	REQUIRE( doc.set_string( "messages", "not-an-array" ) );

	CHECK( doc.make_array_at( "messages" ) == nullptr );

	auto still = doc.get_string( "messages" );
	REQUIRE( static_cast< bool >( still ) );
	CHECK( *still == "not-an-array" );
}
