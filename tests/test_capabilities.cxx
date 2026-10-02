#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "mcode/model/capabilities.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/toml.hxx"

using namespace mcode;


TEST_CASE( "a config entry prices a model the table does not carry", "[capabilities]" ) {
	// the id carries `/` and `.`, so it is quoted and taken verbatim as the entry key.
	REQUIRE_FALSE( model::resolve_capabilities( "cb/gpt-5.6-sol", nullptr ).has_value( ) );

	auto parsed = toml::parse( R"(
[models."cb/gpt-5.6-sol"]
caching = "implicit"
context_window = 400000
max_output_tokens = 128000
price_input = 1.25
price_cached_read = 0.125
price_output = 10.0
supports_thinking = true
)" );

	REQUIRE( static_cast< bool >( parsed ) );

	auto layers = std::vector< config::layer >{ };
	layers.push_back( { .level = config::scope::user, .origin = { }, .values = std::move( *parsed ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	const auto caps = model::resolve_capabilities( "cb/gpt-5.6-sol", &*merged );
	REQUIRE( caps.has_value( ) );

	if ( caps ) {
		REQUIRE( caps->model == "cb/gpt-5.6-sol" );
		REQUIRE( caps->price_input == 1.25 );
		REQUIRE( caps->price_output == 10.0 );
		REQUIRE( caps->context_window == 400'000 );
		REQUIRE( caps->max_output_tokens == 128'000 );
		REQUIRE( caps->caching == model::cache_mode::implicit );
		REQUIRE( caps->supports_thinking );

		REQUIRE( caps->supports_tool_calls );

		REQUIRE( caps->price_cached_read == 0.125 );
	}

	// no cached-read price: input price stands in, as a gateway may bill all input at that rate.
	{
		auto plain = toml::parse( R"(
[models.plain]
price_input = 7.0
price_output = 21.0
)" );
		REQUIRE( static_cast< bool >( plain ) );

		auto plain_layers = std::vector< config::layer >{ };
		plain_layers.push_back( { .level = config::scope::user, .origin = { },
			.values = std::move( *plain ) } );

		auto plain_merged = config::merged_config::merge( std::move( plain_layers ) );
		REQUIRE( static_cast< bool >( plain_merged ) );

		const auto plain_caps = model::resolve_capabilities( "plain", &*plain_merged );
		REQUIRE( plain_caps.has_value( ) );

		if ( plain_caps ) {
			REQUIRE( plain_caps->price_cached_read == 7.0 );
		}
	}

	// an entry that names no price is refused: it would price every turn at zero.
	auto unpriced = toml::parse( R"(
[models.cheap]
supports_thinking = true
)" );
	REQUIRE( static_cast< bool >( unpriced ) );

	auto unpriced_layers = std::vector< config::layer >{ };
	unpriced_layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *unpriced ) } );

	auto unpriced_merged = config::merged_config::merge( std::move( unpriced_layers ) );
	REQUIRE( static_cast< bool >( unpriced_merged ) );
	REQUIRE_FALSE( model::resolve_capabilities( "cheap", &*unpriced_merged ).has_value( ) );

	auto integral = toml::parse( R"(
[models.flat]
price_input = 2
price_output = 4
)" );
	REQUIRE( static_cast< bool >( integral ) );

	auto integral_layers = std::vector< config::layer >{ };
	integral_layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *integral ) } );

	auto integral_merged = config::merged_config::merge( std::move( integral_layers ) );
	REQUIRE( static_cast< bool >( integral_merged ) );

	const auto flat = model::resolve_capabilities( "flat", &*integral_merged );
	REQUIRE( flat.has_value( ) );

	if ( flat ) {
		REQUIRE( flat->price_input == 2.0 );
		REQUIRE( flat->price_output == 4.0 );
	}
}

TEST_CASE( "an unknown model id does not price as free", "[capabilities]" ) {
	REQUIRE_FALSE( model::lookup_capabilities( "totally-made-up-model" ).has_value( ) );
	REQUIRE_FALSE( model::lookup_capabilities( "" ).has_value( ) );

	const auto known = model::lookup_capabilities( "claude-sonnet-4-5" );
	REQUIRE( known.has_value( ) );

	if ( known ) {
		REQUIRE( known->price_input == 3.0 );
		REQUIRE( known->price_output == 15.0 );
		REQUIRE( known->caching == model::cache_mode::explicit_markers );
		REQUIRE( known->context_window == 200'000 );
	}
}
