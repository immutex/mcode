#include "mcode/mcp/jsonrpc.hxx"

#include "mcode/support/json.hxx"

#include <utility>

namespace mcode::mcp::jsonrpc {

	namespace {

		// only an integer or string id can be echoed back; a null, bool or fractional id cannot
		[[nodiscard]] auto id_from( const json::document& doc ) -> std::optional< request_id > {
			if ( const auto number = doc.pointer_int( "/id" ) ) {
				if ( *number >= 0 ) {
					return request_id::numeric( static_cast< std::uint64_t >( *number ) );
				}

				return std::nullopt;
			}

			if ( const auto text = doc.pointer_string( "/id" ) ) {
				return request_id::string( *text );
			}

			return std::nullopt;
		}

		// a reply echoes the request's id verbatim: a string stays a string
		auto set_id( json::document& doc, const request_id& id ) -> status {
			if ( id.is_string ) {
				return doc.set_string( "id", id.text );
			}

			return doc.set_int( "id", static_cast< std::int64_t >( id.number ) );
		}

	}

	auto request_id::numeric( const std::uint64_t value ) -> request_id {
		auto out = request_id{ };
		out.number = value;

		return out;
	}

	auto request_id::string( std::string value ) -> request_id {
		auto out = request_id{ };
		out.is_string = true;
		out.text = std::move( value );

		return out;
	}

	auto stamp_meta( const std::string_view params_json ) -> result< std::string > {
		// a request without params still needs a map to hold `_meta`
		auto text = params_json.empty( ) ? std::string{ "{}" } : std::string{ params_json };

		auto parsed = json::document::parse( text );

		if ( !parsed ) {
			return std::unexpected( fail( errc::json,
				"params are not valid JSON: " + parsed.error( ).msg ) );
		}

		if ( parsed->has_pointer( "/_meta" ) ) {
			return text;
		}

		auto meta = json::document::make_object( );

		if ( const auto attached = meta.make_object_at( "_meta" ); attached == nullptr ) {
			return std::unexpected( fail( errc::json, "could not build the _meta map" ) );
		}

		auto merged = json::document::make_object( );

		// params' own members first, so a server-supplied `_meta` survives the merge
		for ( const auto& key : parsed->keys_at( "" ) ) {
			auto value = json::node_at( *parsed, "/" + key );

			if ( !value ) {
				return std::unexpected( value.error( ) );
			}

			if ( const auto written = merged.set_node( key, std::move( *value ) ); !written ) {
				return std::unexpected( written.error( ) );
			}
		}

		auto meta_node = json::node_from_json( meta.dump( false ).value_or( "{}" ) );

		if ( !meta_node ) {
			return std::unexpected( meta_node.error( ) );
		}

		// the node is the map's body; attaching the node yields { "_meta": { "_meta": {} } }
		const auto* body = meta_node->member( "_meta" );

		if ( body == nullptr ) {
			return std::unexpected( fail( errc::json, "the _meta map lost its key" ) );
		}

		if ( const auto written = merged.set_node( "_meta", *body ); !written ) {
			return std::unexpected( written.error( ) );
		}

		auto dumped = merged.dump( false );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		return *dumped;
	}

	auto render_request( const std::uint64_t id, const std::string_view method,
		const std::string_view params_json ) -> result< std::string > {
		auto doc = json::document::make_object( );

		if ( const auto set = doc.set_string( "jsonrpc", "2.0" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = doc.set_int( "id", static_cast< std::int64_t >( id ) ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = doc.set_string( "method", method ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( !params_json.empty( ) ) {
			if ( const auto set = doc.set_json( "params", params_json ); !set ) {
				return std::unexpected( set.error( ) );
			}
		}

		auto dumped = doc.dump( false );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		return *dumped;
	}

	auto render_notification( const std::string_view method,
		const std::string_view params_json ) -> result< std::string > {
		auto doc = json::document::make_object( );

		if ( const auto set = doc.set_string( "jsonrpc", "2.0" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = doc.set_string( "method", method ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( !params_json.empty( ) ) {
			if ( const auto set = doc.set_json( "params", params_json ); !set ) {
				return std::unexpected( set.error( ) );
			}
		}

		auto dumped = doc.dump( false );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		return *dumped;
	}

	auto render_response( const request_id& id, const std::string_view result_json )
		-> result< std::string > {
		auto doc = json::document::make_object( );

		if ( const auto set = doc.set_string( "jsonrpc", "2.0" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = set_id( doc, id ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( !result_json.empty( ) ) {
			if ( const auto set = doc.set_json( "result", result_json ); !set ) {
				return std::unexpected( set.error( ) );
			}
		} else if ( const auto set = doc.set_json( "result", "null" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		auto dumped = doc.dump( false );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		return *dumped;
	}

	auto render_error_response( const request_id& id, const int code,
		const std::string_view reason ) -> result< std::string > {
		auto doc = json::document::make_object( );

		if ( const auto set = doc.set_string( "jsonrpc", "2.0" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = set_id( doc, id ); !set ) {
			return std::unexpected( set.error( ) );
		}

		auto body = json::node::make_object( );
		body.members[ "code" ] = json::node::make_integer( code );
		body.members[ "message" ] = json::node::make_string( reason );

		if ( const auto set = doc.set_node( "error", std::move( body ) ); !set ) {
			return std::unexpected( set.error( ) );
		}

		auto dumped = doc.dump( false );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		return *dumped;
	}

	auto parse_line( const std::string_view line ) -> result< std::optional< message > > {
		if ( line.size( ) > MAX_FRAME_BYTES ) {
			return std::unexpected( fail( errc::protocol, "frame exceeds the size bound" ) );
		}

		auto parsed = json::document::parse( line );

		if ( !parsed ) {
			return std::optional< message >{ };
		}

		if ( const auto version = parsed->get_string( "jsonrpc" ); !version || *version != "2.0" ) {
			return std::optional< message >{ };
		}

		auto frame = message{ };

		if ( parsed->has_pointer( "/method" ) ) {
			auto method = parsed->get_string( "method" );

			if ( !method ) {
				return std::optional< message >{ };
			}

			frame.method = *method;

			if ( parsed->has_pointer( "/id" ) ) {
				const auto id = id_from( *parsed );

				if ( !id ) {
					return std::optional< message >{ };
				}

				frame.kind = message_kind::request;
				frame.id = *id;
			} else {
				frame.kind = message_kind::notification;
			}

			if ( parsed->has_pointer( "/params" ) ) {
				auto params = parsed->pointer_raw( "/params" );

				if ( !params ) {
					return std::unexpected( params.error( ) );
				}

				frame.params_json = *params;
			}

			return std::optional< message >{ std::move( frame ) };
		}

		const auto id = id_from( *parsed );

		if ( !id ) {
			return std::optional< message >{ };
		}

		frame.kind = message_kind::response;
		frame.id = *id;

		if ( parsed->has_pointer( "/error" ) ) {
			frame.is_error = true;

			auto body = parsed->pointer_raw( "/error" );

			if ( !body ) {
				return std::unexpected( body.error( ) );
			}

			frame.body_json = *body;
		} else if ( parsed->has_pointer( "/result" ) ) {
			auto body = parsed->pointer_raw( "/result" );

			if ( !body ) {
				return std::unexpected( body.error( ) );
			}

			frame.body_json = *body;
		} else {
			return std::optional< message >{ };
		}

		return std::optional< message >{ std::move( frame ) };
	}

}
