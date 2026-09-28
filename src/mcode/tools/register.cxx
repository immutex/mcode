#include "mcode/tools/register.hxx"

#include <string>

#include "mcode/support/json.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/file_tools.hxx"
#include "mcode/tools/schemas.hxx"
#include "mcode/tools/search_tools.hxx"

namespace mcode::tools {

	namespace {

		// Validates the subset of JSON-Schema the core tools use: type object,
		// properties, required naming declared properties.
		[[nodiscard]] auto validate_schema_object( const json::document& schema,
			std::string& complaint ) -> bool {
			const auto type = schema.get_string( "type" );

			if ( !type || *type != "object" ) {
				complaint = "type must be \"object\"";

				return false;
			}

			const auto required = schema.pointer_string_array( "/required" );

			if ( required ) {
				for ( const auto& name : *required ) {
					const auto declared = schema.pointer( "/properties/" + name );

					if ( !declared ) {
						complaint = "required names an undeclared property: " + name;

						return false;
					}
				}
			}

			return true;
		}

		// Wraps a raw handler with the argument-parse step and the error contract,
		// so no tool hand-rolls its own malformed-argument path.
		auto wrap_handler( const std::function< result< std::string >( const tool_args&,
			tool_context& ) >& raw, const tool_context& context )
			-> std::function< result< std::string >( std::string_view args_json ) > {
			return [ raw, context ]( const std::string_view args_json ) -> result< std::string > {
				auto parsed = tool_args::parse( args_json );

				if ( !parsed ) {
					return argument_error( parsed.error( ).msg );
				}

				auto mutable_context = context;

				return raw( *parsed, mutable_context );
			};
		}

		auto wrap_handler_with_registry(
			const std::function< result< std::string >( const tool_args&, tool_context&,
				const tool_registry& ) >& raw, const tool_context& context,
			const tool_registry& registry )
			-> std::function< result< std::string >( std::string_view args_json ) > {
			return [ raw, context, &registry ]( const std::string_view args_json )
				-> result< std::string > {
				auto parsed = tool_args::parse( args_json );

				if ( !parsed ) {
					return argument_error( parsed.error( ).msg );
				}

				auto mutable_context = context;

				return raw( *parsed, mutable_context, registry );
			};
		}

	}

	auto validate_schema( const std::string_view schema_json ) -> status {
		auto parsed = json::document::parse( schema_json );

		if ( !parsed ) {
			return std::unexpected( fail( errc::json,
				"schema is not valid JSON: " + parsed.error( ).msg ) );
		}

		auto complaint = std::string{ };

		if ( !validate_schema_object( *parsed, complaint ) ) {
			return std::unexpected( fail( errc::json, "invalid schema: " + complaint ) );
		}

		return { };
	}

	auto register_core_tools( tool_registry& registry, tool_handler_sink& sink,
		const tool_context& context ) -> status {
		const struct {
			std::string_view name;
			std::string_view description;
			tool_class klass;
			std::string_view schema;
		} CORE[] = {
			{ "read", "Read a file window with line numbers; refuses binary with a stub", tool_class::read, READ_SCHEMA },
			{ "edit", "Replace an exact string in a file and return a unified diff", tool_class::write, EDIT_SCHEMA },
			{ "write", "Create or overwrite a file; overwrite requires a prior read", tool_class::write, WRITE_SCHEMA },
			{ "glob", "Find files by pattern, ignoring .git and build output", tool_class::read, GLOB_SCHEMA },
			{ "grep", "Search file contents by regex, capped and binary-skipping", tool_class::read, GREP_SCHEMA },
			{ "bash", "Run a single shell command behind the approval policy", tool_class::exec, BASH_SCHEMA },
			{ "ask_user", "Ask the user a question and return the answer", tool_class::read, ASK_USER_SCHEMA },
			{ "tool_search", "Find tools by name or description; expand for full schemas", tool_class::read, TOOL_SEARCH_SCHEMA },
		};

		for ( const auto& entry : CORE ) {
			auto schema_status = validate_schema( entry.schema );

			if ( !schema_status ) {
				return std::unexpected( fail( errc::json,
					"core schema for " + std::string{ entry.name } + " is invalid: " +
					schema_status.error( ).msg ) );
			}

			auto definition = tool_def{ };
			definition.name = std::string{ entry.name };
			definition.description = std::string{ entry.description };
			definition.klass = entry.klass;
			definition.source = tool_source::core;
			definition.deferrable = false;
			definition.schema_json = std::string{ entry.schema };

			if ( auto added = registry.add( std::move( definition ) ); !added ) {
				return std::unexpected( added.error( ) );
			}
		}

		// Handlers go through the sink after every schema is registered, so a
		// schema failure leaves no half-registered set behind.
		sink.add_handler( "read", wrap_handler( handle_read, context ) );
		sink.add_handler( "write", wrap_handler( handle_write, context ) );
		sink.add_handler( "edit", wrap_handler( handle_edit, context ) );
		sink.add_handler( "glob", wrap_handler( handle_glob, context ) );
		sink.add_handler( "grep", wrap_handler( handle_grep, context ) );
		sink.add_handler( "bash", wrap_handler( handle_bash, context ) );
		sink.add_handler( "ask_user", wrap_handler( handle_ask_user, context ) );
		sink.add_handler( "tool_search",
			wrap_handler_with_registry( handle_tool_search, context, registry ) );

		return { };
	}

}
