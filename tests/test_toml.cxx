#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "mcode/support/parse.hxx"
#include "mcode/support/toml.hxx"

using namespace mcode;

TEST_CASE( "toml parses the subset config and manifests use", "[toml]" ) {
	auto parsed = toml::parse( R"(
# a comment
name = "git"
version = "0.3.0"
api_version = 1
enabled = true
timeout_ms = 5000
ratio = 0.25
permissions = ["fs_read", "spawn"]

[stream.usage]
in = "/usage/prompt_tokens"
out = "/usage/completion_tokens"
)" );

	REQUIRE( static_cast< bool >( parsed ) );

	REQUIRE( parsed->get_string( "name" ) == std::string{ "git" } );
	REQUIRE( parsed->get_int( "api_version" ) == 1 );
	REQUIRE( parsed->get_bool( "enabled" ) == true );

	auto permissions = parsed->get_string_array( "permissions" );
	REQUIRE( static_cast< bool >( permissions ) );
	REQUIRE( permissions->size( ) == 2 );
	REQUIRE( ( *permissions )[ 0 ] == "fs_read" );

	// a `[table]` header and a dotted key flatten to the same key.
	REQUIRE( parsed->get_string( "stream.usage.in" ) == std::string{ "/usage/prompt_tokens" } );
	REQUIRE( parsed->contains( "stream.usage.out" ) );

	// absence, a present value, and a wrong type are three different answers.
	REQUIRE_FALSE( parsed->contains( "nope" ) );

	auto absent = parsed->optional_string( "nope" );
	REQUIRE( static_cast< bool >( absent ) );

	if ( absent ) {
		REQUIRE_FALSE( absent->has_value( ) );
	}

	auto present = parsed->optional_string( "name" );
	REQUIRE( static_cast< bool >( present ) );

	if ( present ) {
		REQUIRE( present->has_value( ) );
	}

	// a wrong type is an error, not an absence.
	auto mistyped = parsed->optional_int( "version" );
	REQUIRE_FALSE( static_cast< bool >( mistyped ) );

	if ( !mistyped ) {
		REQUIRE( mistyped.error( ).msg.find( "not an integer" ) != std::string::npos );
	}
}

TEST_CASE( "toml refuses what it cannot represent rather than ignoring it", "[toml]" ) {
	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"(when = 1979-05-27T07:32:00Z)", "date" },
		{ R"(point = { x = 1, y = 2 })", "inline table" },
		{ R"(text = """multi""")", "multi-line string" },
		{ R"(bad = "unterminated)", "unterminated string" },
		{ R"(name = )", "missing value" },
		{ R"(name)", "no equals" },
		{ R"(name = "a"
name = "b")", "duplicate key" },
		{ R"(mixed = ["a", 1])", "heterogeneous array" },
		{ R"(bad = "\q")", "unsupported escape" },
		{ R"([unclosed)", "unterminated table header" },
		{ R"(name = "a" trailing)", "text after a value" },

		// a malformed number must be refused, not truncated to its numeric prefix.
		{ R"(ratio = 0.25.9)", "a number with two dots" },
		{ R"(ratio = 1e)", "an exponent with no digits" },
		{ R"(ratio = 1.2e5.6)", "an exponent followed by more digits" },
	};

	for ( const auto& [ text, label ] : cases ) {
		auto parsed = toml::parse( text );

		CHECK_FALSE( static_cast< bool >( parsed ) );

		if ( parsed ) {
			FAIL( "case '" << label << "' parsed but should not have" );
		}
	}
}

TEST_CASE( "a parse error names the line", "[toml]" ) {
	auto parsed = toml::parse( "ok = 1\nok = 2\n" );
	REQUIRE_FALSE( static_cast< bool >( parsed ) );

	REQUIRE( parsed.error( ).msg.find( "line 2" ) != std::string::npos );
}

TEST_CASE( "a UTF-8 BOM does not make the file unreadable", "[toml]" ) {
	// notepad writes a UTF-8 BOM; without the skip the first byte is not a space, a '#', or a '['.
	auto parsed = toml::parse( "\xEF\xBB\xBFmodel = \"gpt\"\n" );

	REQUIRE( static_cast< bool >( parsed ) );

	if ( !parsed ) {
		FAIL( parsed.error( ).msg );
	}

	auto model = parsed->get_string( "model" );
	REQUIRE( model.has_value( ) );

	if ( model ) {
		REQUIRE( *model == "gpt" );
	}

	// a mid-file BOM is content, not a marker; concatenated because `\xBFbad` is one hex escape.
	const auto bom_mid_file = std::string{ "ok = 1\n" } + "\xEF\xBB\xBF" + "bad = 2\n";

	CHECK_FALSE( static_cast< bool >( toml::parse( bom_mid_file ) ) );
}

TEST_CASE( "the float parser accepts and refuses exactly what it says", "[toml]" ) {
	// the compiler's float from_chars is missing on Xcode 15, so this parser is the implementation.
	auto value = 0.0;

	for ( const auto& [ text, expected ] : std::vector< std::pair< const char*, double > >{
		{ "0", 0.0 },
		{ "1", 1.0 },
		{ "-1", -1.0 },
		{ "+2", 2.0 },
		{ "0.25", 0.25 },
		{ "-0.25", -0.25 },
		{ "1.5e3", 1500.0 },
		{ "1.5E3", 1500.0 },
		{ "1e-3", 0.001 },
		{ "1.5e+3", 1500.0 },
		{ "123456789012345678", 123456789012345678.0 },
		{ "0.0", 0.0 },
		{ "100", 100.0 },
	} ) {
		CHECK( support::parse_double( text, value ) );
		CHECK( value == expected );
	}

	for ( const auto* text : {
		"", "+", "-", ".", ".5", "5.", "e5", "1e", "1e+", "1E-",
		"0.25.9", "1.2e5.6", "1.2.3", "abc", "1a", "1 ", " 1", "1_000",
		"0x10", "inf", "nan", "--1", "1..2", "1e99999", "1e-99999",
		"1234567890123456789", "1.7976931348623157e309",
	} ) {
		CHECK_FALSE( support::parse_double( text, value ) );
	}

	// underflow to zero is refused, not reported as 0.
	CHECK_FALSE( support::parse_double( "1e-400", value ) );
}

TEST_CASE( "the float parser rounds once, not per decade", "[toml]" ) {
	auto value = 0.0;

	REQUIRE( support::parse_double( "3.14", value ) );
	CHECK( value == 3.14 );

	REQUIRE( support::parse_double( "1e100", value ) );
	CHECK( value == 1e100 );

	REQUIRE( support::parse_double( "1.7976931348623157e308", value ) );
	CHECK( value == 1.7976931348623157e308 );

	// DBL_MIN is a normal double even though 10^-324 itself underflows
	REQUIRE( support::parse_double( "2.2250738585072014e-308", value ) );
	CHECK( value == 2.2250738585072014e-308 );

	// a zero significand stays zero whatever the exponent
	REQUIRE( support::parse_double( "0e400", value ) );
	CHECK( value == 0.0 );
}
