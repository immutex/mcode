#include "mcode/tools/tool_args.hxx"

#include <string>

#include "mcode/tools/errors.hxx"
#include "mcode/tools/json_repair.hxx"
#include "mcode/tools/schema_check.hxx"

namespace mcode::tools {

	namespace {

		inline constexpr std::string_view EMPTY_ARGUMENTS = "{}";

		inline constexpr std::string_view RECOVERY_HINT =
			"send one JSON object as the arguments, for example {\"path\":\"src/main.cxx\"}; "
			"tool_search returns the full schema when a parameter name is unclear";

	}

	auto prepare_arguments( const std::string_view schema_json, const std::string_view args_json )
		-> prepared_arguments {
		auto out = prepared_arguments{ };

		const auto text = args_json.empty( ) ? EMPTY_ARGUMENTS : args_json;

		auto repaired = repair_json( text );

		if ( !repaired ) {
			out.failure = argument_error( std::string{ "the arguments are not a JSON object: " } +
				repaired.error( ).msg, RECOVERY_HINT );

			return out;
		}

		// An empty schema is an unconstrained tool: an MCP server or an extension may register one.
		if ( !schema_json.empty( ) ) {
			auto faults = check_arguments( schema_json, *repaired );

			if ( !faults ) {
				out.failure = argument_error( faults.error( ).msg, RECOVERY_HINT );

				return out;
			}

			if ( !faults->empty( ) ) {
				out.failure = argument_error( describe_faults( *faults ), RECOVERY_HINT );

				return out;
			}
		}

		out.json = std::move( *repaired );

		return out;
	}

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
