#include "mcode/tools/schema_check.hxx"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::tools {

	namespace {

		// A property name as a JSON Pointer segment.
		//
		// The name comes straight from the model's arguments, and a pointer gives
		// `~` and `/` meaning: a key of `a/b` resolved to the nested node
		// `properties.a.b` and `a~1b` to `properties.a/b`, so an argument was
		// validated against a different property's type and enum -- a bypass of
		// both `additionalProperties: false` and enum membership. RFC 6901 escapes
		// `~` first, or the `~1` produced by `/` would be re-escaped.
		[[nodiscard]] auto pointer_segment( const std::string_view name ) -> std::string {
			auto out = std::string{ };
			out.reserve( name.size( ) );

			for ( const auto character : name ) {
				if ( character == '~' ) {
					out += "~0";
				} else if ( character == '/' ) {
					out += "~1";
				} else {
					out.push_back( character );
				}
			}

			return out;
		}

		[[nodiscard]] auto is_named_type( const json::node& value, const std::string_view type )
			-> bool {
			if ( type == "string" ) {
				return value.type == json::node::kind::string;
			}

			if ( type == "integer" ) {
				return value.type == json::node::kind::integer;
			}

			if ( type == "number" ) {
				return value.type == json::node::kind::integer ||
					value.type == json::node::kind::real;
			}

			if ( type == "boolean" ) {
				return value.type == json::node::kind::boolean;
			}

			if ( type == "array" ) {
				return value.type == json::node::kind::array;
			}

			if ( type == "object" ) {
				return value.type == json::node::kind::object;
			}

			if ( type == "null" ) {
				return value.type == json::node::kind::null_value;
			}

			// an unrecognized type name constrains nothing, so it cannot fail a call.
			return true;
		}

		[[nodiscard]] auto observed_type( const json::node& value ) -> std::string_view {
			switch ( value.type ) {
				case json::node::kind::null_value: return "null";
				case json::node::kind::boolean: return "boolean";
				case json::node::kind::integer: return "integer";
				case json::node::kind::real: return "number";
				case json::node::kind::string: return "string";
				case json::node::kind::array: return "array";
				case json::node::kind::object: return "object";
			}

			return "unknown";
		}

		[[nodiscard]] auto quote_join( const std::vector< std::string >& values ) -> std::string {
			auto out = std::string{ };

			for ( const auto& value : values ) {
				if ( !out.empty( ) ) {
					out += ", ";
				}

				out += '"';
				out += value;
				out += '"';
			}

			return out;
		}

		[[nodiscard]] auto declares_no_additional_properties( const json::document& schema )
			-> bool {
			const auto value = schema.pointer_bool( "/additionalProperties" );

			return value.has_value( ) && !*value;
		}

	}

	auto check_arguments( const std::string_view schema_json, const std::string_view args_json )
		-> result< std::vector< argument_fault > > {
		auto schema = json::document::parse( schema_json );

		if ( !schema ) {
			return std::unexpected( fail( errc::json,
				"tool schema is not JSON: " + schema.error( ).msg ) );
		}

		auto arguments = json::document::parse( args_json );

		if ( !arguments ) {
			return std::unexpected( fail( errc::json,
				"tool arguments are not JSON: " + arguments.error( ).msg ) );
		}

		auto root = arguments->root_node( );

		if ( !root ) {
			return std::unexpected( root.error( ) );
		}

		auto faults = std::vector< argument_fault >{ };

		if ( root->type != json::node::kind::object ) {
			faults.push_back( argument_fault{ .parameter = "<arguments>",
				.problem = "is " + std::string{ observed_type( *root ) } +
					", but the tool takes an object",
				.admissible = "a JSON object" } );

			return faults;
		}

		const auto declared = schema->keys_at( "/properties" );
		const auto required = schema->pointer_string_array( "/required" );

		for ( const auto& name : required.value_or( std::vector< std::string >{ } ) ) {
			if ( !root->member( name ) ) {
				faults.push_back( argument_fault{ .parameter = name,
					.problem = "is required but missing",
					.admissible = quote_join( declared ) } );
			}
		}

		for ( const auto& [key, value] : root->members ) {
			// Escaped: the key is the model's own, and a pointer gives `/` and `~`
			// meaning, so an unescaped one resolves to a different property.
			const auto escaped = pointer_segment( key );
			const auto property = schema->pointer_string( "/properties/" + escaped + "/type" );

			if ( !property || property->empty( ) ) {
				if ( !schema->has_pointer( "/properties/" + escaped ) &&
					declares_no_additional_properties( *schema ) ) {
					faults.push_back( argument_fault{ .parameter = key,
						.problem = "is not a parameter of this tool",
						.admissible = quote_join( declared ) } );
				}

				continue;
			}

			if ( value.type == json::node::kind::null_value ) {
				continue;
			}

			if ( !is_named_type( value, *property ) ) {
				faults.push_back( argument_fault{ .parameter = key,
					.problem = "is " + std::string{ observed_type( value ) } + ", expected " +
						*property,
					.admissible = std::string{ } } );

				continue;
			}

			const auto choices = schema->pointer_string_array( "/properties/" + escaped + "/enum" );

			if ( !choices || choices->empty( ) ) {
				continue;
			}

			if ( std::find( choices->begin( ), choices->end( ), value.text ) == choices->end( ) ) {
				faults.push_back( argument_fault{ .parameter = key,
					.problem = "is \"" + value.text + "\", which is not one of the allowed values",
					.admissible = quote_join( *choices ) } );
			}
		}

		return faults;
	}

	auto describe_faults( const std::vector< argument_fault >& faults ) -> std::string {
		auto out = std::string{ };

		for ( const auto& fault : faults ) {
			if ( !out.empty( ) ) {
				out += "; ";
			}

			out += fault.parameter;
			out += ' ';
			out += fault.problem;

			if ( !fault.admissible.empty( ) ) {
				out += " (admissible: ";
				out += fault.admissible;
				out += ')';
			}
		}

		return out;
	}

}
