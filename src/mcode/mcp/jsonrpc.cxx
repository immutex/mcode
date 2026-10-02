#include "mcode/mcp/jsonrpc.hxx"

#include "mcode/support/json.hxx"

namespace mcode::mcp::jsonrpc {

	namespace {

		// a hostile or broken server can send one enormous line; this keeps the parse bounded
		inline constexpr std::size_t MAX_FRAME_BYTES = 8u * 1024u * 1024u;

		[[nodiscard]] auto id_from( const json::document& doc ) -> std::optional< std::uint64_t > {
			// a numeric or string id both carry; a non-numeric string must not correlate as zero
			if ( doc.has_pointer( "/id" ) ) {
				if ( const auto number = doc.pointer_int( "/id" ) ) {
					if ( *number >= 0 ) {
						return static_cast< std::uint64_t >( *number );
					}

					return std::nullopt;
				}

				if ( const auto text = doc.pointer_string( "/id" ) ) {
					auto number = std::uint64_t{ 0 };
					auto digits = true;

					for ( const auto character : *text ) {
						if ( character < '0' || character > '9' ) {
							digits = false;

							break;
						}

						number = number * 10u +
							static_cast< std::uint64_t >( character - '0' );
					}

					if ( digits && !text->empty( ) ) {
						return number;
					}
				}

				return std::nullopt;
			}

			return std::nullopt;
		}

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

	auto render_response( const std::uint64_t id, const std::string_view result_json )
		-> result< std::string > {
		auto doc = json::document::make_object( );

		if ( const auto set = doc.set_string( "jsonrpc", "2.0" ); !set ) {
			return std::unexpected( set.error( ) );
		}

		if ( const auto set = doc.set_int( "id", static_cast< std::int64_t >( id ) ); !set ) {
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

			if ( const auto id = id_from( *parsed ) ) {
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
