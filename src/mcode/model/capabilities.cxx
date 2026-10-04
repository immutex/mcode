#include "mcode/model/capabilities.hxx"

#include "mcode/support/json.hxx"
#include "mcode/support/parse.hxx"

namespace mcode::model {

	namespace {

		// USD per million tokens from the providers' published pages; an absent entry fails closed.
		const char* CAPABILITIES_JSON = R"JSON([
	{
		"model": "ali/deepseek-v4.1-flash",
		"caching": "implicit",
		"supports_tool_calls": true,
		"supports_strict_schema": false,
		"supports_response_schema": false,
		"supports_thinking": true,
		"supports_effort": false,
		"context_window": 1000000,
		"max_output_tokens": 393216,
		"price_input": 0.15,
		"price_cached_read": 0.015,
		"price_cache_write": 0.15,
		"price_output": 0.6
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
			out.cached_read_in_input = parse_bool( entry, "cached_read_in_input", true );
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

	auto capability_config_section( const std::string_view model_id ) -> std::string {
		return "[models.\"" + std::string{ model_id } + "\"]";
	}

	auto resolve_capabilities( const std::string_view model_id,
		const mcode::config::merged_config* from_config ) -> std::optional< capabilities > {
		auto caps = lookup_capabilities( model_id );

		if ( from_config == nullptr ) {
			return caps;
		}

		// no transform on the id: a quoted key segment flattens verbatim, so the id is the key.
		const auto prefix = "models." + std::string{ model_id } + '.';
		auto merged = caps.value_or( capabilities{ } );

		if ( const auto value = from_config->get_string( prefix + "caching" ) ) {
			merged.caching = parse_caching( *value );
		}

		if ( const auto value = from_config->get_bool( prefix + "cached_read_in_input" ) ) {
			merged.cached_read_in_input = *value;
		}

		if ( const auto value = from_config->get_int( prefix + "context_window" ) ) {
			merged.context_window = *value;
		}

		if ( const auto value = from_config->get_int( prefix + "max_output_tokens" ) ) {
			merged.max_output_tokens = *value;
		}

		if ( const auto value = from_config->get_bool( prefix + "supports_tool_calls" ) ) {
			merged.supports_tool_calls = *value;
		}

		if ( const auto value = from_config->get_bool( prefix + "supports_strict_schema" ) ) {
			merged.supports_strict_schema = *value;
		}

		if ( const auto value = from_config->get_bool( prefix + "supports_response_schema" ) ) {
			merged.supports_response_schema = *value;
		}

		if ( const auto value = from_config->get_bool( prefix + "supports_thinking" ) ) {
			merged.supports_thinking = *value;
		}

		if ( const auto value = from_config->get_bool( prefix + "supports_effort" ) ) {
			merged.supports_effort = *value;
		}

		if ( const auto value = from_config->get_double( prefix + "price_input" ) ) {
			merged.price_input = *value;
		}

		const auto cached_read = from_config->get_double( prefix + "price_cached_read" );

		if ( cached_read ) {
			merged.price_cached_read = *cached_read;
		}

		if ( const auto value = from_config->get_double( prefix + "price_cache_write" ) ) {
			merged.price_cache_write = *value;
		}

		if ( const auto value = from_config->get_double( prefix + "price_output" ) ) {
			merged.price_output = *value;
		}

		// a config entry declares the model known, so one with no price at all is refused.
		if ( !caps && merged.price_input <= 0.0 && merged.price_output <= 0.0 ) {
			return std::nullopt;
		}

		// neither config nor table priced a cached read: charge it at input rather than guess low.
		if ( !cached_read && merged.price_cached_read <= 0.0 && merged.price_input > 0.0 ) {
			merged.price_cached_read = merged.price_input;
		}

		merged.model = std::string{ model_id };

		return merged;
	}

}
