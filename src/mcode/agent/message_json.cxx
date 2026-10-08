#include "mcode/agent/message_json.hxx"

#include <array>

#include "mcode/support/json.hxx"

namespace mcode::agent {

	namespace {

		inline constexpr std::array< std::string_view, 4 > ROLE_NAMES{
			"system", "user", "assistant", "tool" };

		inline constexpr std::array< std::string_view, 4 > BLOCK_NAMES{
			"text", "thinking", "tool_call", "tool_result" };

		[[nodiscard]] auto role_name( const model::role value ) -> std::string_view {
			const auto index = static_cast< std::size_t >( value );

			return index < ROLE_NAMES.size( ) ? ROLE_NAMES[ index ] : "user";
		}

		[[nodiscard]] auto block_name( const model::block_kind value ) -> std::string_view {
			const auto index = static_cast< std::size_t >( value );

			return index < BLOCK_NAMES.size( ) ? BLOCK_NAMES[ index ] : "text";
		}

		[[nodiscard]] auto role_from_name( const std::string_view name )
			-> std::optional< model::role > {
			for ( auto index = std::size_t{ 0 }; index < ROLE_NAMES.size( ); ++index ) {
				if ( ROLE_NAMES[ index ] == name ) {
					return static_cast< model::role >( index );
				}
			}

			return std::nullopt;
		}

		[[nodiscard]] auto block_from_name( const std::string_view name )
			-> std::optional< model::block_kind > {
			for ( auto index = std::size_t{ 0 }; index < BLOCK_NAMES.size( ); ++index ) {
				if ( BLOCK_NAMES[ index ] == name ) {
					return static_cast< model::block_kind >( index );
				}
			}

			return std::nullopt;
		}

		// `keys_at` enumerates an OBJECT's keys, so it returns nothing for an
		// array. There is no length accessor, so the elements are probed until
		// one is absent. Arrays here are short (a message's blocks, a history's
		// messages), so the linear probe costs nothing.
		[[nodiscard]] auto array_length( const json::document& document,
			const std::string_view path ) -> std::size_t {
			auto count = std::size_t{ 0 };

			for ( ;; ) {
				const auto probe = std::string{ path } + "/" + std::to_string( count );

				if ( !document.has_pointer( probe ) ) {
					return count;
				}

				++count;
			}
		}

		auto append_string_member( std::string& out, const std::string_view key,
			const std::string_view value, const bool first ) -> void {
			if ( !first ) {
				out += ',';
			}

			out += '"';
			json::append_escaped( out, key );
			out += "\":\"";
			json::append_escaped( out, value );
			out += '"';
		}

	}

	auto message_to_json( const model::message& message ) -> std::string {
		auto out = std::string{ "{\"role\":\"" };
		out += role_name( message.speaker );
		out += "\",\"blocks\":[";

		auto first_block = true;

		for ( const auto& block : message.blocks ) {
			if ( !first_block ) {
				out += ',';
			}

			first_block = false;

			out += "{\"kind\":\"";
			out += block_name( block.kind );
			out += '"';

			// Only the fields the kind actually carries are written, so a text
			// block does not grow four empty strings in the log.
			switch ( block.kind ) {
				case model::block_kind::text:
				case model::block_kind::thinking:
					append_string_member( out, "text", block.text, false );

					break;

				case model::block_kind::tool_call:
					append_string_member( out, "id", block.tool_call_id, false );
					append_string_member( out, "name", block.tool_name, false );
					append_string_member( out, "args", block.args_json, false );

					break;

				case model::block_kind::tool_result:
					append_string_member( out, "id", block.tool_call_id, false );
					append_string_member( out, "result", block.result_json, false );
					out += ",\"is_error\":";
					out += block.is_error ? "true" : "false";

					break;
			}

			out += '}';
		}

		out += "]}";

		return out;
	}

	auto message_from_json( const std::string_view json_text ) -> result< model::message > {
		auto document = json::document::parse( json_text );

		if ( !document ) {
			return std::unexpected( document.error( ) );
		}

		auto role = document->get_string( "role" );

		if ( !role ) {
			return std::unexpected( fail( errc::json, "message has no role" ) );
		}

		const auto speaker = role_from_name( *role );

		if ( !speaker ) {
			return std::unexpected( fail( errc::json, "unknown message role: " + *role ) );
		}

		auto message = model::message{ };
		message.speaker = *speaker;

		// RFC 6901 pointers, so the separator is `/` and not `.`: `yyjson` walks
		// the document by pointer and a dotted path simply never matches.
		const auto block_count = array_length( *document, "/blocks" );

		for ( auto index = std::size_t{ 0 }; index < block_count; ++index ) {
			const auto path = std::string{ "/blocks/" } + std::to_string( index );
			auto kind = document->pointer_string( path + "/kind" );

			if ( !kind ) {
				continue;
			}

			const auto decoded = block_from_name( *kind );

			if ( !decoded ) {
				return std::unexpected( fail( errc::json, "unknown block kind: " + *kind ) );
			}

			auto block = model::block{ };
			block.kind = *decoded;

			const auto text = [&]( const std::string_view member ) -> std::string {
				const auto value = document->pointer_string( path + "/" + std::string{ member } );

				return value ? *value : std::string{ };
			};

			switch ( block.kind ) {
				case model::block_kind::text:
				case model::block_kind::thinking:
					block.text = text( "text" );

					break;

				case model::block_kind::tool_call:
					block.tool_call_id = text( "id" );
					block.tool_name = text( "name" );
					block.args_json = text( "args" );

					break;

				case model::block_kind::tool_result:
					block.tool_call_id = text( "id" );
					block.result_json = text( "result" );

					if ( const auto flag = document->pointer_bool( path + "/is_error" ) ) {
						block.is_error = *flag;
					}

					break;
			}

			message.blocks.push_back( std::move( block ) );
		}

		return message;
	}

	auto history_to_json( const std::vector< model::message >& history ) -> std::string {
		auto out = std::string{ "{\"messages\":[" };

		for ( auto index = std::size_t{ 0 }; index < history.size( ); ++index ) {
			if ( index > 0 ) {
				out += ',';
			}

			out += message_to_json( history[ index ] );
		}

		out += "]}";

		return out;
	}

	auto history_from_json( const std::string_view json_text )
		-> result< std::vector< model::message > > {
		auto document = json::document::parse( json_text );

		if ( !document ) {
			return std::unexpected( document.error( ) );
		}

		auto history = std::vector< model::message >{ };
		const auto count = array_length( *document, "/messages" );

		for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
			const auto raw = document->pointer_raw(
				std::string{ "/messages/" } + std::to_string( index ) );

			if ( !raw ) {
				continue;
			}

			auto message = message_from_json( *raw );

			if ( !message ) {
				return std::unexpected( message.error( ) );
			}

			history.push_back( std::move( *message ) );
		}

		return history;
	}

}
