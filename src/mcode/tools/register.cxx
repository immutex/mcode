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

		inline constexpr std::string_view RECOVERY_HINT =
			"send one JSON object as the arguments, for example {\"path\":\"src/main.cxx\"}; "
			"tool_search returns the full schema when a parameter name is unclear";

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

			// Flatness is a measured reliability property, not a style preference: nesting
			// arguments costs every model tens of points of call accuracy, so a nested object
			// property is refused here rather than shipped to the model.
			for ( const auto& name : schema.keys_at( "/properties" ) ) {
				const auto property = schema.pointer_string( "/properties/" + name + "/type" );

				if ( !property ) {
					continue;
				}

				if ( *property == "object" ) {
					complaint = "property '" + name +
						"' is a nested object; tool parameters must be flat - split the tool or "
						"flatten the parameter";

					return false;
				}

				if ( *property == "array" ) {
					const auto items = schema.pointer_string( "/properties/" + name + "/items/type" );

					if ( items && *items == "object" ) {
						complaint = "property '" + name +
							"' is an array of objects; tool parameters must be flat";

						return false;
					}
				}
			}

			return true;
		}

		auto wrap_handler( const std::function< result< std::string >( const tool_args&,
			tool_context& ) >& raw, const tool_context& context )
			-> std::function< result< std::string >( std::string_view args_json ) > {
			return [ raw, context ]( const std::string_view args_json ) -> result< std::string > {
				auto parsed = tool_args::parse( args_json );

				if ( !parsed ) {
					return argument_error( parsed.error( ).msg, RECOVERY_HINT );
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
					return argument_error( parsed.error( ).msg, RECOVERY_HINT );
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
			{ "read", "Read a file window with line numbers; refuses binary with a stub. Do not use "
				"to list files (use glob) or to search contents (use grep).",
				tool_class::read, READ_SCHEMA },
			{ "edit", "Replace an exact string in a file and return a unified diff. The file must "
				"have been read this session, and old_string must be unique in it. Do not use to "
				"create a file or to rewrite one wholesale (use write).",
				tool_class::write, EDIT_SCHEMA },
			{ "write", "Create or overwrite a file; overwrite requires a prior read. Do not use to "
				"change part of a file (use edit) or to append.",
				tool_class::write, WRITE_SCHEMA },
			{ "glob", "Find files by pattern, ignoring .git and build output. Do not use to read a "
				"file (use read) or to match file contents (use grep).",
				tool_class::read, GLOB_SCHEMA },
			{ "grep", "Search file contents by regex, capped and binary-skipping. Do not use to "
				"find files by name (use glob), and do not use it to read a file you already know "
				"the path of.",
				tool_class::read, GREP_SCHEMA },
			{ "bash", "Run a single shell command behind the approval policy. Use it only when no "
				"other tool does the job: do not use it to read, search or write files (read, "
				"grep, glob, edit and write are faster and are not gated). Compound commands are "
				"refused.",
				tool_class::exec, BASH_SCHEMA },
			{ "ask_user", "Ask the user a question and return the answer. Use it only when the "
				"answer changes what you do next and cannot be found in the repository; do not "
				"use it to confirm a step you can verify yourself.",
				tool_class::read, ASK_USER_SCHEMA },
			{ "tool_search", "Find tools by name or description; expand for full schemas. Do not "
				"call it before every tool call: the core tools are already in your tool list.",
				tool_class::read, TOOL_SEARCH_SCHEMA },
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

		// after every schema, so a schema failure leaves no half-registered set behind
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
