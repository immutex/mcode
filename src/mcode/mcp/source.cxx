#include "mcode/mcp/source.hxx"

#include "mcode/tools/register.hxx"

#include <utility>

namespace mcode::mcp {

	namespace {

		inline constexpr std::string_view NAME_SEPARATOR = "__";
		inline constexpr std::string_view OWNER_PREFIX = "mcp:";

		auto owner_of( const std::string& server_name ) -> std::string {
			return std::string{ OWNER_PREFIX } + server_name;
		}

	}

	auto source::register_tool( const std::string& server_name, const server_tool& tool )
		-> result< void > {
		auto schema_status = tools::validate_schema( tool.schema_json );

		if ( !schema_status ) {
			return std::unexpected( fail( errc::json,
				"server " + server_name + " tool " + tool.name +
				" has an invalid schema: " + schema_status.error( ).msg ) );
		}

		auto definition = tool_def{ };
		definition.name = qualified_tool_name( server_name, tool.name );
		definition.description = wrap_untrusted( tool.description );
		definition.klass = tool_class::mcp;
		definition.source = tool_source::mcp;
		definition.owner = owner_of( server_name );
		definition.schema_json = tool.schema_json;

		if ( auto added = registry_->add( std::move( definition ) ); !added ) {
			return std::unexpected( added.error( ) );
		}

		return { };
	}

	auto source::register_server( const std::string& server_name,
		const std::vector< server_tool >& tools ) -> result< void > {
		// A restart re-registers the same names. The registry rejects duplicate
		// names loudly, so the server's prior entries are removed first; when
		// there are none, the removal is a no-op rather than an error.
		registry_->remove_owner( owner_of( server_name ) );

		for ( const auto& tool : tools ) {
			auto registered = register_tool( server_name, tool );

			if ( !registered ) {
				return std::unexpected( registered.error( ) );
			}
		}

		return { };
	}

	auto source::unregister_server( const std::string& server_name ) -> std::size_t {
		return registry_->remove_owner( owner_of( server_name ) );
	}

	auto qualified_tool_name( const std::string& server_name,
		const std::string& tool_name ) -> std::string {
		auto out = std::string{ "mcp" };
		out += NAME_SEPARATOR;
		out += server_name;
		out += NAME_SEPARATOR;
		out += tool_name;

		return out;
	}

}
