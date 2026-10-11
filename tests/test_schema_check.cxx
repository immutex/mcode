#include <catch2/catch_test_macros.hpp>

#include "mcode/tools/schema_check.hxx"

using namespace mcode;

TEST_CASE( "a property name with a slash is not a pointer into another property",
	"[tools][schema]" ) {
	// A JSON Pointer gives `/` and `~` meaning, so an unescaped key of `a/b` resolved
	// into `properties.a` and validated the argument against a different property's
	// type.
	const auto schema = R"({
		"type": "object",
		"properties": {
			"a": { "type": "string", "enum": ["ok"] }
		},
		"additionalProperties": false
	})";

	const auto faults = tools::check_arguments( schema, R"({ "a/b": 123 })" );

	REQUIRE( static_cast< bool >( faults ) );
	REQUIRE( faults->size( ) == 1 );
	CHECK( faults->front( ).parameter == "a/b" );
	CHECK( faults->front( ).problem.find( "not a parameter" ) != std::string::npos );
}

TEST_CASE( "a tilde in a property name is not a pointer escape", "[tools][schema]" ) {
	// `~1` spells `/`, so an unescaped `~` lets one property name address another.
	const auto schema = R"({
		"type": "object",
		"properties": {
			"a": { "type": "string" }
		},
		"additionalProperties": false
	})";

	const auto faults = tools::check_arguments( schema, R"({ "a~1b": "x" })" );

	REQUIRE( static_cast< bool >( faults ) );
	REQUIRE( faults->size( ) == 1 );
	CHECK( faults->front( ).parameter == "a~1b" );
}

TEST_CASE( "an ordinary property still validates", "[tools][schema]" ) {
	// The escape must not have broken the names it leaves alone.
	const auto schema = R"({
		"type": "object",
		"properties": {
			"count": { "type": "integer" },
			"mode": { "type": "string", "enum": ["fast", "slow"] }
		},
		"required": ["count"],
		"additionalProperties": false
	})";

	const auto good = tools::check_arguments( schema, R"({ "count": 3, "mode": "fast" })" );

	REQUIRE( static_cast< bool >( good ) );
	CHECK( good->empty( ) );

	const auto wrong_type = tools::check_arguments( schema, R"({ "count": "three" })" );

	REQUIRE( static_cast< bool >( wrong_type ) );
	REQUIRE( wrong_type->size( ) == 1 );
	CHECK( wrong_type->front( ).parameter == "count" );

	const auto missing = tools::check_arguments( schema, R"({ "mode": "fast" })" );

	REQUIRE( static_cast< bool >( missing ) );
	REQUIRE( missing->size( ) == 1 );
	CHECK( missing->front( ).problem.find( "required but missing" ) != std::string::npos );

	const auto bad_enum = tools::check_arguments( schema, R"({ "count": 1, "mode": "medium" })" );

	REQUIRE( static_cast< bool >( bad_enum ) );
	REQUIRE( bad_enum->size( ) == 1 );
	CHECK( bad_enum->front( ).parameter == "mode" );
}
