#include "mcode/mcp/client.hxx"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mcode/mcp/constants.hxx"
#include "mcode/mcp/content.hxx"
#include "mcode/mcp/jsonrpc.hxx"
#include "mcode/support/json.hxx"

namespace mcode::mcp {

	namespace {

		// Both listings share the cursor protocol, including the two ways it can fail to
		// terminate. A server that loops would otherwise block startup, one request per
		// page, at DEFAULT_LIST_TIMEOUT each.
		[[nodiscard]] auto page_cursor( const std::string_view member_name,
			const std::optional< std::string >& cursor, std::string& previous )
			-> result< std::string > {
			if ( !cursor ) {
				return std::string{ };
			}

			if ( *cursor == previous ) {
				return std::unexpected( fail( errc::protocol, std::string{ member_name }
					+ " repeated its cursor, so the listing cannot terminate" ) );
			}

			previous = *cursor;

			return R"({"cursor":)" + detail::json_escape( *cursor ) + "}";
		}

		[[nodiscard]] auto read_member_string( const json::node& entry,
			const std::string_view name ) -> std::string {
			if ( const auto* value = entry.member( name ); value != nullptr &&
				value->type == json::node::kind::string ) {
				return value->text;
			}

			return { };
		}

		[[nodiscard]] auto parse_resource( const json::node& entry ) -> server_resource {
			auto resource = server_resource{ };
			resource.uri = read_member_string( entry, "uri" );
			resource.name = read_member_string( entry, "name" );
			resource.description = read_member_string( entry, "description" );
			resource.mime_type = read_member_string( entry, "mimeType" );

			return resource;
		}

		[[nodiscard]] auto parse_prompt( const json::node& entry ) -> server_prompt {
			auto prompt = server_prompt{ };
			prompt.name = read_member_string( entry, "name" );
			prompt.description = read_member_string( entry, "description" );

			if ( const auto* arguments = entry.member( "arguments" ); arguments != nullptr &&
				arguments->type == json::node::kind::array ) {
				for ( const auto& value : arguments->items ) {
					auto argument = prompt_argument{ };
					argument.name = read_member_string( value, "name" );
					argument.description = read_member_string( value, "description" );

					if ( const auto* required = value.member( "required" );
						required != nullptr && required->type == json::node::kind::boolean ) {
						argument.required = required->boolean;
					}

					prompt.arguments.push_back( std::move( argument ) );
				}
			}

			return prompt;
		}

	}

	auto client::list_resources( ) -> result< std::vector< server_resource > > {
		auto all = std::vector< server_resource >{ };
		auto cursor = std::optional< std::string >{ };
		auto previous = std::string{ };
		auto pages = std::size_t{ 0 };

		while ( true ) {
			if ( pages >= MAX_TOOL_PAGES ) {
				return std::unexpected( fail( errc::protocol, "resources/list did not "
					"terminate within " + std::to_string( MAX_TOOL_PAGES ) + " pages" ) );
			}

			++pages;

			const auto params = page_cursor( "resources/list", cursor, previous );

			if ( !params ) {
				return std::unexpected( params.error( ) );
			}

			auto body = call( jsonrpc::RESOURCES_LIST, *params, DEFAULT_LIST_TIMEOUT );

			if ( !body ) {
				return std::unexpected( std::move( body ).error( ) );
			}

			auto parsed = json::document::parse( *body );

			if ( !parsed ) {
				return std::unexpected( std::move( parsed ).error( ) );
			}

			auto root = parsed->root_node( );

			if ( !root ) {
				return std::unexpected( root.error( ) );
			}

			if ( const auto* list = root->member( "resources" ); list != nullptr &&
				list->type == json::node::kind::array ) {
				for ( const auto& entry : list->items ) {
					all.push_back( parse_resource( entry ) );
				}
			}

			const auto next = read_member_string( *root, "nextCursor" );

			if ( next.empty( ) ) {
				break;
			}

			cursor = next;
		}

		return all;
	}

	auto client::list_prompts( ) -> result< std::vector< server_prompt > > {
		auto all = std::vector< server_prompt >{ };
		auto cursor = std::optional< std::string >{ };
		auto previous = std::string{ };
		auto pages = std::size_t{ 0 };

		while ( true ) {
			if ( pages >= MAX_TOOL_PAGES ) {
				return std::unexpected( fail( errc::protocol, "prompts/list did not "
					"terminate within " + std::to_string( MAX_TOOL_PAGES ) + " pages" ) );
			}

			++pages;

			const auto params = page_cursor( "prompts/list", cursor, previous );

			if ( !params ) {
				return std::unexpected( params.error( ) );
			}

			auto body = call( jsonrpc::PROMPTS_LIST, *params, DEFAULT_LIST_TIMEOUT );

			if ( !body ) {
				return std::unexpected( std::move( body ).error( ) );
			}

			auto parsed = json::document::parse( *body );

			if ( !parsed ) {
				return std::unexpected( std::move( parsed ).error( ) );
			}

			auto root = parsed->root_node( );

			if ( !root ) {
				return std::unexpected( root.error( ) );
			}

			if ( const auto* list = root->member( "prompts" ); list != nullptr &&
				list->type == json::node::kind::array ) {
				for ( const auto& entry : list->items ) {
					all.push_back( parse_prompt( entry ) );
				}
			}

			const auto next = read_member_string( *root, "nextCursor" );

			if ( next.empty( ) ) {
				break;
			}

			cursor = next;
		}

		return all;
	}

	auto client::read_resource( const std::string_view uri ) -> result< fetched_text > {
		const auto params = std::string{ R"({"uri":)" } + detail::json_escape( uri ) + "}";

		auto body = call( jsonrpc::RESOURCES_READ, params, DEFAULT_CALL_TIMEOUT );

		if ( !body ) {
			return std::unexpected( std::move( body ).error( ) );
		}

		auto text = detail::flatten_content( *body );

		if ( !text ) {
			return std::unexpected( std::move( text ).error( ) );
		}

		return fetched_text{ .text = std::move( *text ),
			.mime_type = detail::content_mime_type( *body ) };
	}

	auto client::get_prompt( const std::string_view name,
		const std::string_view arguments_json ) -> result< fetched_text > {
		auto params = std::string{ R"({"name":)" } + detail::json_escape( name );
		params += R"(,"arguments":)";
		params += arguments_json.empty( ) ? std::string{ "{}" }
			: std::string{ arguments_json };
		params += "}";

		auto body = call( jsonrpc::PROMPTS_GET, params, DEFAULT_CALL_TIMEOUT );

		if ( !body ) {
			return std::unexpected( std::move( body ).error( ) );
		}

		auto text = detail::flatten_content( *body );

		if ( !text ) {
			return std::unexpected( std::move( text ).error( ) );
		}

		return fetched_text{ .text = std::move( *text ),
			.mime_type = detail::content_mime_type( *body ) };
	}

}
