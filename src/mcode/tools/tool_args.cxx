#include "mcode/tools/tool_args.hxx"

#include <string>

namespace mcode::tools {

	auto tool_args::parse( const std::string_view args_json ) -> result< tool_args > {
		auto parsed = json::document::parse( args_json );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		auto args = tool_args{ };
		args.document_ = std::move( *parsed );

		return args;
	}

	auto tool_args::string_field( const std::string_view key ) const
		-> std::optional< std::string > {
		auto value = document_.get_string( key );

		if ( value ) {
			return *value;
		}

		return std::nullopt;
	}

	auto tool_args::int_field( const std::string_view key ) const -> std::optional< std::int64_t > {
		auto value = document_.pointer_int( "/" + std::string{ key } );

		if ( value ) {
			return *value;
		}

		return std::nullopt;
	}

	auto tool_args::bool_field( const std::string_view key ) const -> std::optional< bool > {
		auto value = document_.pointer_bool( "/" + std::string{ key } );

		if ( value ) {
			return *value;
		}

		return std::nullopt;
	}

	auto tool_args::string_array_field( const std::string_view key ) const
		-> std::optional< std::vector< std::string > > {
		auto value = document_.pointer_string_array( "/" + std::string{ key } );

		if ( value ) {
			return *value;
		}

		return std::nullopt;
	}

}
