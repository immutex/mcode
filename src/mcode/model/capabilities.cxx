#include "mcode/model/capabilities.hxx"

#include "mcode/support/json.hxx"
#include "mcode/support/parse.hxx"

namespace mcode::model {

	namespace {

		// Prices are USD per million tokens, from the providers' published pages
		// at the time they were recorded. Only sourced entries are in the table:
		// an invented price is a fabricated number, and an absent entry fails
		// closed at lookup rather than pricing a run at zero.
		const char* CAPABILITIES_JSON = R"JSON([
	{
		"model": "claude-sonnet-4-5",
		"caching": "explicit_markers",
		"supports_tool_calls": true,
		"supports_strict_schema": false,
		"supports_response_schema": false,
		"supports_thinking": true,
		"supports_effort": false,
		"context_window": 200000,
		"max_output_tokens": 64000,
		"price_input": 3.0,
		"price_cached_read": 0.3,
		"price_cache_write": 3.75,
		"price_output": 15.0
	},
	{
		"model": "claude-haiku-4-5",
		"caching": "explicit_markers",
		"supports_tool_calls": true,
		"supports_strict_schema": false,
		"supports_response_schema": false,
		"supports_thinking": true,
		"supports_effort": false,
		"context_window": 200000,
		"max_output_tokens": 64000,
		"price_input": 1.0,
		"price_cached_read": 0.1,
		"price_cache_write": 1.25,
		"price_output": 5.0
	},
	{
		"model": "claude-opus-4-1",
		"caching": "explicit_markers",
		"supports_tool_calls": true,
		"supports_strict_schema": false,
		"supports_response_schema": false,
		"supports_thinking": true,
		"supports_effort": false,
		"context_window": 200000,
		"max_output_tokens": 32000,
		"price_input": 15.0,
		"price_cached_read": 1.5,
		"price_cache_write": 18.75,
		"price_output": 75.0
	},
	{
		"model": "gpt-5",
		"caching": "implicit",
		"supports_tool_calls": true,
		"supports_strict_schema": true,
		"supports_response_schema": true,
		"supports_thinking": false,
		"supports_effort": true,
		"context_window": 400000,
		"max_output_tokens": 128000,
		"price_input": 1.25,
		"price_cached_read": 0.125,
		"price_cache_write": 0.0,
		"price_output": 10.0
	},
	{
		"model": "gpt-5-mini",
		"caching": "implicit",
		"supports_tool_calls": true,
		"supports_strict_schema": true,
		"supports_response_schema": true,
		"supports_thinking": false,
		"supports_effort": true,
		"context_window": 400000,
		"max_output_tokens": 128000,
		"price_input": 0.25,
		"price_cached_read": 0.025,
		"price_cache_write": 0.0,
		"price_output": 2.0
	}
])JSON";

		[[nodiscard]] auto parse_bool( const json::node& entry, const std::string_view key,
			const bool fallback ) -> bool {
			const auto found = entry.members.find( std::string{ key } );

			if ( found == entry.members.end( ) || found->second.type != json::node::kind::boolean ) {
				return fallback;
			}

			return found->second.boolean;
		}

		[[nodiscard]] auto parse_int( const json::node& entry, const std::string_view key )
			-> std::int64_t {
			const auto found = entry.members.find( std::string{ key } );

			return ( found != entry.members.end( ) && found->second.type == json::node::kind::integer )
				? found->second.integer
				: 0;
		}

		[[nodiscard]] auto parse_real( const json::node& entry, const std::string_view key )
			-> double {
			const auto found = entry.members.find( std::string{ key } );

			if ( found == entry.members.end( ) ) {
				return 0.0;
			}

			if ( found->second.type == json::node::kind::real ) {
				return found->second.real;
			}

			if ( found->second.type == json::node::kind::integer ) {
				return static_cast< double >( found->second.integer );
			}

			return 0.0;
		}

		[[nodiscard]] auto parse_caching( const std::string_view text ) -> cache_mode {
			if ( text == "explicit_markers" ) {
				return cache_mode::explicit_markers;
			}

			if ( text == "implicit" ) {
				return cache_mode::implicit;
			}

			return cache_mode::none;
		}

		[[nodiscard]] auto parse_string( const json::node& entry, const std::string_view key )
			-> std::string {
			const auto found = entry.members.find( std::string{ key } );

			return ( found != entry.members.end( ) && found->second.type == json::node::kind::string )
				? found->second.text
				: std::string{ };
		}

		[[nodiscard]] auto parse_entry( const std::map< std::string, json::node, std::less<> >& members )
			-> std::optional< capabilities > {
			const auto id = members.find( "model" );

			if ( id == members.end( ) || id->second.type != json::node::kind::string
				|| id->second.text.empty( ) ) {
				return std::nullopt;
			}

			auto entry = json::node::make_object( );
			entry.members = members;

			auto out = capabilities{ };
			out.model = id->second.text;
			out.caching = parse_caching( parse_string( entry, "caching" ) );
			out.supports_tool_calls = parse_bool( entry, "supports_tool_calls", true );
			out.supports_strict_schema = parse_bool( entry, "supports_strict_schema", false );
			out.supports_response_schema = parse_bool( entry, "supports_response_schema", false );
			out.supports_thinking = parse_bool( entry, "supports_thinking", false );
			out.supports_effort = parse_bool( entry, "supports_effort", false );
			out.context_window = parse_int( entry, "context_window" );
			out.max_output_tokens = parse_int( entry, "max_output_tokens" );
			out.price_input = parse_real( entry, "price_input" );
			out.price_cached_read = parse_real( entry, "price_cached_read" );
			out.price_cache_write = parse_real( entry, "price_cache_write" );
			out.price_output = parse_real( entry, "price_output" );

			return out;
		}

	}

	auto lookup_capabilities( const std::string_view model_id ) -> std::optional< capabilities > {
		auto table = json::document::parse( CAPABILITIES_JSON );

		if ( !table ) {
			return std::nullopt;
		}

		for ( auto index = std::size_t{ 0 };; ++index ) {
			const auto prefix = "/" + std::to_string( index );

			auto id = table->pointer_string( prefix + "/model" );

			if ( !id ) {
				break;
			}

			if ( *id != model_id ) {
				continue;
			}

			auto entry = json::node_at( *table, prefix );

			if ( !entry || entry->type != json::node::kind::object ) {
				return std::nullopt;
			}

			return parse_entry( entry->members );
		}

		return std::nullopt;
	}

}
