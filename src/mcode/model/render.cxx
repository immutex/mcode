#include "mcode/model/render.hxx"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

		inline constexpr std::string_view RENDERABLE_SHAPE = "openai-chat-completions";

		[[nodiscard]] auto render_message( const message& source, const request_spec& fields )
			-> result< json::node > {
			auto out = json::node::make_object( );

			out.members[ "role" ] = json::node::make_string( to_string( source.speaker ) );

			auto content = std::string{ };
			auto calls = json::node::make_array( );

			for ( const auto& piece : source.blocks ) {
				switch ( piece.kind ) {
					case block_kind::text:
					case block_kind::thinking:
						content += piece.text;
						break;

					case block_kind::tool_call: {
						auto entry = json::node::make_object( );
						entry.members[ "id" ] = json::node::make_string( piece.tool_call_id );
						entry.members[ "type" ] = json::node::make_string( "function" );

						auto& function = entry.members[ "function" ];
						function = json::node::make_object( );
						function.members[ "name" ] = json::node::make_string( piece.tool_name );
						function.members[ "arguments" ] =
							json::node::make_string( piece.args_json );

						calls.items.push_back( std::move( entry ) );
						break;
					}

					case block_kind::tool_result: {
						auto entry = json::node::make_object( );
						entry.members[ "role" ] = json::node::make_string( fields.role_tool );
						entry.members[ "tool_call_id" ] =
							json::node::make_string( piece.tool_call_id );
						entry.members[ "content" ] = json::node::make_string( piece.result_json );

						out.members[ "role" ] = json::node::make_string( fields.role_tool );
						out.members[ "tool_call_id" ] =
							json::node::make_string( piece.tool_call_id );
						out.members[ "content" ] = json::node::make_string( piece.result_json );

						return out;
					}
				}
			}

			out.members[ "content" ] = json::node::make_string( content );

			if ( !calls.items.empty( ) ) {
				out.members[ "tool_calls" ] = std::move( calls );
			}

			return out;
		}

		// sorted by name: the array's byte order is part of the cache prefix.
		[[nodiscard]] auto render_tools( const std::vector< tool_spec >& tools )
			-> result< json::node > {
			auto sorted = tools;
			std::sort( sorted.begin( ), sorted.end( ),
				[]( const tool_spec& left, const tool_spec& right ) {
					return left.name < right.name;
				} );

			auto out = json::node::make_array( );

			for ( const auto& tool : sorted ) {
				auto schema = json::node_from_json( tool.schema_json );

				if ( !schema ) {
					return std::unexpected( fail( errc::json,
						"tool '" + tool.name + "' has a schema that is not JSON: " +
						schema.error( ).msg ) );
				}

				auto entry = json::node::make_object( );
				entry.members[ "type" ] = json::node::make_string( "function" );

				auto& function = entry.members[ "function" ];
				function = json::node::make_object( );
				function.members[ "name" ] = json::node::make_string( tool.name );
				function.members[ "description" ] = json::node::make_string( tool.description );
				function.members[ "parameters" ] = std::move( *schema );

				out.items.push_back( std::move( entry ) );
			}

			return out;
		}

		[[nodiscard]] auto effort_string( const effort value ) -> std::string_view {
			switch ( value ) {
				case effort::low: return "low";
				case effort::medium: return "medium";
				case effort::high: return "high";
				case effort::provider_default: return "";
			}

			return "";
		}

		// applied right to left: a marker occupies bytes, so each offset stays valid when used.
		inline constexpr std::string_view CACHE_MARKER =
			"\"cache_control\":{\"type\":\"ephemeral\"},";

		// The marker is inserted at a member position, so the anchor must be the byte after the
		// opening brace of the first message object - the system message, which is the last
		// byte of the stable prefix. Offsets are into the rendered body, so only the renderer
		// can locate one: the message key cannot occur inside an earlier value, and the tools
		// array follows the messages, so the first match is the real one.
		[[nodiscard]] auto stable_prefix_offsets( const std::string_view body,
			const request_spec& fields ) -> std::vector< std::size_t > {
			const auto key = "\"" + fields.messages + "\":";
			const auto key_at = body.find( key );

			if ( key_at == std::string_view::npos ) {
				return { };
			}

			const auto array_at = body.find( '[', key_at + key.size( ) );

			if ( array_at == std::string_view::npos || body.compare( array_at, 2, "[{" ) != 0 ) {
				return { };
			}

			return { array_at + 2 };
		}

		[[nodiscard]] auto apply_breakpoints( std::string body,
			const std::vector< std::size_t >& offsets ) -> result< std::string > {
			auto applied = offsets;
			std::sort( applied.begin( ), applied.end( ), std::greater< std::size_t >{ } );

			for ( const auto offset : applied ) {
				if ( offset > body.size( ) ) {
					return std::unexpected( fail( errc::protocol,
						"cache breakpoint " + std::to_string( offset ) +
						" is past the rendered body" ) );
				}

				body.insert( offset, CACHE_MARKER );
			}

			return body;
		}

	}

	auto is_renderable_shape( const provider_descriptor& descriptor ) noexcept -> bool {
		return descriptor.name == RENDERABLE_SHAPE;
	}

	namespace {

		// The unmarked body. Breakpoints are offsets into it, so both entry points below build
		// from this one function and the offsets can never be located on a marker-shifted body.
		[[nodiscard]] auto render_body( const chat_request& request, const request_spec& fields )
			-> result< std::string > {
			auto body = json::node::make_object( );

			body.members[ fields.model ] = json::node::make_string( request.model );

			auto& messages = body.members[ fields.messages ];
			messages = json::node::make_array( );

			for ( const auto& entry : request.messages ) {
				auto rendered = render_message( entry, fields );

				if ( !rendered ) {
					return std::unexpected( rendered.error( ) );
				}

				messages.items.push_back( std::move( *rendered ) );
			}

			if ( !request.tools.empty( ) ) {
				auto tools = render_tools( request.tools );

				if ( !tools ) {
					return std::unexpected( tools.error( ) );
				}

				body.members[ fields.tools ] = std::move( *tools );
			}

			if ( request.max_output_tokens > 0 ) {
				body.members[ fields.max_output_tokens ] =
					json::node::make_integer( request.max_output_tokens );
			}

			if ( request.temperature >= 0.0 ) {
				body.members[ fields.temperature ] = json::node::make_real( request.temperature );
			}

			if ( request.stream ) {
				body.members[ "stream" ] = json::node::make_boolean( true );
			}

			if ( !request.response_schema_json.empty( ) ) {
				body.members[ fields.response_schema ] =
					json::node::make_string( request.response_schema_json );
			}

			if ( request.reasoning_effort != effort::provider_default ) {
				body.members[ "reasoning_effort" ] =
					json::node::make_string( effort_string( request.reasoning_effort ) );
			}

			auto built = json::document::make_object( );

			for ( auto& [ key, value ] : body.members ) {
				if ( !built.set_node( key, std::move( value ) ) ) {
					return std::unexpected( fail( errc::json,
						"failed to build the request body" ) );
				}
			}

			auto text = built.dump( false );

			if ( !text ) {
				return std::unexpected( text.error( ) );
			}

			return *text;
		}

	} // namespace

	auto render_request( const stream_request& request ) -> result< std::string > {
		if ( !is_renderable_shape( request.provider ) ) {
			return std::unexpected( fail( errc::unsupported,
				"provider '" + request.provider.name + "' uses a request shape this build "
				"cannot render; only the chat-completions shape is supported" ) );
		}

		return render_chat_completions( request.request, request.provider.request );
	}

	auto render_chat_completions( const chat_request& request, const request_spec& fields )
		-> result< std::string > {
		auto text = render_body( request, fields );

		if ( !text ) {
			return text;
		}

		// The markers are the caller's: the assembler fills them, because a byte offset into
		// the rendered body is not something a caller can compute from the messages alone.
		if ( request.cache.mode == cache_mode::explicit_markers &&
			!request.cache.breakpoints.empty( ) ) {
			return apply_breakpoints( std::move( *text ), request.cache.breakpoints );
		}

		return *text;
	}

	auto cache_breakpoints( const chat_request& request, const request_spec& fields )
		-> std::vector< std::size_t > {
		// An already-populated plan is the caller's decision; only an empty one is filled.
		if ( !request.cache.breakpoints.empty( ) ) {
			return request.cache.breakpoints;
		}

		auto text = render_body( request, fields );

		if ( !text ) {
			return { };
		}

		return stable_prefix_offsets( *text, fields );
	}

}
