#include "mcode/mcp/client.hxx"

#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"
#include "mcode/mcp/constants.hxx"

#include <utility>

namespace mcode::mcp {

	namespace {

		auto json_escape( const std::string_view text ) -> std::string {
			auto out = std::string{ "\"" };
			json::append_escaped( out, text );
			out.push_back( '"' );

			return out;
		}

		auto initialize_params_json( ) -> std::string {
			auto caps = json::node::make_object( );
			caps.members[ "tools" ] = json::node::make_object( );

			auto info = json::node::make_object( );
			info.members[ "name" ] = json::node::make_string( CLIENT_NAME );
			info.members[ "version" ] = json::node::make_string( CLIENT_VERSION );

			auto doc = json::node::make_object( );
			doc.members[ "capabilities" ] = std::move( caps );
			doc.members[ "clientInfo" ] = std::move( info );
			doc.members[ "protocolVersion" ] = json::node::make_string( PROTOCOL_VERSION );

			auto rendered = json::document::make_object( );

			if ( const auto written = rendered.set_node( "root", std::move( doc ) ); !written ) {
				return { };
			}

			return rendered.pointer_raw( "/root" ).value_or( "{}" );
		}

		auto cancel_params( const std::uint64_t id ) -> std::string {
			return R"({"reason":"timeout","requestId":)" + std::to_string( id ) + "}";
		}

		// A tools/list page. `next_cursor` is present when the server has more.
		struct list_page {
			std::vector< server_tool > tools;
			std::string next_cursor;
		};

		auto parse_tool( const json::node& entry ) -> result< server_tool > {
			auto tool = server_tool{ };

			if ( const auto* name = entry.member( "name" ); name != nullptr &&
				name->type == json::node::kind::string ) {
				tool.name = name->text;
			} else {
				return std::unexpected( fail( errc::protocol, "a tool entry has no name" ) );
			}

			if ( const auto* description = entry.member( "description" ); description != nullptr &&
				description->type == json::node::kind::string ) {
				tool.description = description->text;
			}

			if ( const auto* schema = entry.member( "inputSchema" ); schema != nullptr ) {
				auto schema_doc = json::document::make_object( );

				if ( const auto written = schema_doc.set_node( "schema", *schema ); !written ) {
					return std::unexpected( written.error( ) );
				}

				// The wrapper document is mutable, so the schema is read back
				// through `dump` and unwrapped; `pointer_raw` only works on a
				// document that came from `parse`.
				auto dumped = schema_doc.dump( false );

				if ( !dumped ) {
					return std::unexpected( dumped.error( ) );
				}

				auto unwrapped = json::document::parse( *dumped );

				if ( !unwrapped ) {
					return std::unexpected( unwrapped.error( ) );
				}

				auto raw = unwrapped->pointer_raw( "/schema" );

				if ( !raw ) {
					return std::unexpected( raw.error( ) );
				}

				tool.schema_json = std::move( *raw );
			}

			return tool;
		}

		auto parse_page( const std::string_view body_json ) -> result< list_page > {
			auto parsed = json::document::parse( body_json );

			if ( !parsed ) {
				return std::unexpected( std::move( parsed ).error( ) );
			}

			auto root = parsed->root_node( );

			if ( !root ) {
				return std::unexpected( root.error( ) );
			}

			auto page = list_page{ };

			if ( const auto* tools = root->member( "tools" ); tools != nullptr &&
				tools->type == json::node::kind::array ) {
				for ( const auto& entry : tools->items ) {
					auto tool = parse_tool( entry );

					if ( !tool ) {
						return std::unexpected( tool.error( ) );
					}

					page.tools.push_back( std::move( *tool ) );
				}
			}

			if ( const auto* cursor = root->member( "nextCursor" ); cursor != nullptr &&
				cursor->type == json::node::kind::string ) {
				page.next_cursor = cursor->text;
			}

			return page;
		}

		// The content blocks of a `tools/call` result, flattened to text. A
		// server may return several blocks; each is appended in order.
		auto flatten_content( const std::string_view result_json ) -> result< std::string > {
			auto parsed = json::document::parse( result_json );

			if ( !parsed ) {
				return std::unexpected( std::move( parsed ).error( ) );
			}

			auto root = parsed->root_node( );

			if ( !root ) {
				return std::unexpected( root.error( ) );
			}

			auto out = std::string{ };

			if ( const auto* content = root->member( "content" ); content != nullptr &&
				content->type == json::node::kind::array ) {
				for ( const auto& block : content->items ) {
					if ( const auto* text = block.member( "text" ); text != nullptr &&
						text->type == json::node::kind::string ) {
						if ( !out.empty( ) ) {
							out.push_back( '\n' );
						}

						out.append( text->text );
					}
				}
			}

			return out;
		}

	}

	auto client::attach( ) -> void {
		wire_->set_callbacks(
			[ this ]( inbound&& item ) {
				handle_line( std::move( item ) );
			},
			[ this ]( ) {
				on_eof( );
			} );
	}

	auto client::handle_line( inbound&& item ) -> void {
		
		auto log_line = []( const std::string_view text ) {
			if ( logging_initialized( ) ) {
				logger( )->warn( "mcp: {}", text );
			}
		};

		if ( item.skipped ) {
			// A banner or a stray log line. Logged, never fatal.
			log_line( "skipping non-JSON-RPC stdout: " + item.line_json );

			return;
		}

		auto parsed = jsonrpc::parse_line( item.line_json );

		if ( !parsed ) {
			
			log_line( "unparseable line: " + parsed.error( ).msg );

			return;
		}

		if ( !parsed->has_value( ) ) {
			

			return;
		}

		auto frame = std::move( **parsed );

		if ( frame.kind == jsonrpc::message_kind::response ) {
			
			const auto found = pending_.find( frame.id );

			if ( found == pending_.end( ) ) {
				// A late response after a timeout or EOF. Observed, counted,
				// ignored -- never delivered as a second result.
				++ignored_responses_;

				return;
			}

			auto& waiter = found->second;

			if ( waiter.done ) {
				return;
			}

			if ( frame.is_error ) {
				waiter.failure = fail( errc::protocol, "server error: " + frame.body_json );
			} else {
				waiter.response_json = std::move( frame.body_json );
			}

			waiter.done = true;

			return;
		}

		if ( frame.kind == jsonrpc::message_kind::notification ) {
			if ( on_notify_ ) {
				on_notify_( frame );
			}

			return;
		}

		// A server-initiated request. This slice advertises no client
		// capabilities that would make a server send one, so the only honest
		// answer is the method-not-found error -- a silent drop would leave the
		// server waiting.
		auto response = jsonrpc::render_response( frame.id,
			R"({"code":-32601,"message":"method not found","data":null})" );

		if ( response ) {
			auto sent = wire_->send( *response );

			if ( !sent && logging_initialized( ) ) {
				logger( )->warn( "mcp: could not answer a server request: {}", sent.error( ).msg );
			}
		}
	}

	auto client::on_eof( ) -> void {
		if ( eof_seen_ ) {
			return;
		}

		eof_seen_ = true;
		alive_ = false;

		failure_.kind = failure_kind::transport;
		failure_.message = "the server's stdout ended";

		fail_pending( errc::io, "the server's stdout ended" );
	}

	auto client::fail_pending( const errc code, std::string message ) -> void {
		for ( auto& [ id, waiter ] : pending_ ) {
			if ( !waiter.done ) {
				waiter.failure = fail( code, message );
				waiter.done = true;
			}
		}
	}

	auto client::send_notification( const std::string_view method,
		const std::string_view params_json ) -> status {
		auto frame = jsonrpc::render_notification( method, params_json );

		if ( !frame ) {
			return std::unexpected( frame.error( ) );
		}

		return wire_->send( *frame );
	}

	auto client::call( const std::string_view method, const std::string_view params_json,
		const std::chrono::milliseconds timeout ) -> result< std::string > {
		if ( eof_seen_ ) {
			return std::unexpected( fail( errc::io, "the server's stdout has ended" ) );
		}

		auto stamped = jsonrpc::stamp_meta( params_json );

		if ( !stamped ) {
			return std::unexpected( std::move( stamped ).error( ) );
		}

		const auto id = next_id_++;
		auto frame = jsonrpc::render_request( id, method, *stamped );

		if ( !frame ) {
			return std::unexpected( frame.error( ) );
		}

		auto& waiter = pending_[ id ];
		waiter.method = std::string{ method };

		auto sent = wire_->send( *frame );

		if ( !sent ) {
			pending_.erase( id );

			return std::unexpected( std::move( sent ).error( ) );
		}

		// A bounded wait: poll the pending entry while the read loop runs. The
		// loop is driven by whoever owns the transport -- `pump` below -- so the
		// timeout is enforced here, at the call site, where the deadline is
		// known.
		const auto deadline = std::chrono::steady_clock::now( ) + timeout;
		auto outcome = result< std::string >{ };

		while ( true ) {
			if ( waiter.done ) {
				if ( waiter.failure ) {
					outcome = std::unexpected( *waiter.failure );
				} else {
					outcome = std::move( *waiter.response_json );
				}

				break;
			}

			if ( eof_seen_ ) {
				outcome = std::unexpected( fail( errc::io, "the server's stdout ended" ) );

				break;
			}

			if ( std::chrono::steady_clock::now( ) >= deadline ) {
				// The absolute maximum is enforced by the caller clamping the
				// timeout; here the per-request deadline fires.
				auto cancelled = send_notification( jsonrpc::CANCELLED_NOTIFICATION,
					cancel_params( id ) );

				if ( !cancelled && logging_initialized( ) ) {
					logger( )->warn( "mcp: could not send the cancellation: {}",
						cancelled.error( ).msg );
				}

				pending_.erase( id );

				outcome = std::unexpected(
					fail( errc::cancelled, "the request timed out: " + std::string{ method } ) );

				break;
			}

			pump( std::chrono::milliseconds{ 10 } );
		}

		pending_.erase( id );

		return outcome;
	}

	auto client::pump( const std::chrono::milliseconds window ) -> void {
		wire_->pump( window );
	}

	auto client::initialize( ) -> result< server_capabilities > {
		auto body = call( jsonrpc::INITIALIZE, initialize_params_json( ),
			ABSOLUTE_MAX_TIMEOUT );

		if ( !body ) {
			return std::unexpected( std::move( body ).error( ) );
		}

		auto parsed = json::document::parse( *body );

		if ( !parsed ) {
			return std::unexpected( std::move( parsed ).error( ) );
		}

		auto negotiated = std::string{ };

		if ( const auto version = parsed->get_string( "protocolVersion" ) ) {
			negotiated = *version;
		} else {
			return std::unexpected(
				fail( errc::protocol, "the initialize response has no protocolVersion" ) );
		}

		if ( negotiated != PROTOCOL_VERSION ) {
			return std::unexpected( fail( errc::unsupported,
				"the server speaks " + negotiated + ", this client speaks " +
				std::string{ PROTOCOL_VERSION } ) );
		}

		auto caps = server_capabilities{ };
		caps.negotiated_version = std::move( negotiated );

		if ( const auto name = parsed->pointer_string( "/serverInfo/name" ) ) {
			caps.server_name = *name;
		}

		if ( const auto version = parsed->pointer_string( "/serverInfo/version" ) ) {
			caps.server_version = *version;
		}

		if ( const auto instructions = parsed->pointer_string( "/instructions" ) ) {
			caps.instructions = *instructions;
		}

		if ( parsed->has_pointer( "/capabilities/tools" ) ) {
			caps.tools = true;
		}

		auto notified = send_notification( jsonrpc::INITIALIZED_NOTIFICATION, "" );

		if ( !notified ) {
			return std::unexpected( std::move( notified ).error( ) );
		}

		caps_ = std::move( caps );
		alive_ = true;

		return caps_;
	}

	auto client::list_tools( ) -> result< std::vector< server_tool > > {
		auto all = std::vector< server_tool >{ };
		auto cursor = std::optional< std::string >{ };

		while ( true ) {
			auto params = std::string{ };

			if ( cursor ) {
				params = R"({"cursor":)" + json_escape( *cursor ) + "}";
			}

			auto body = call( jsonrpc::TOOLS_LIST, params, DEFAULT_LIST_TIMEOUT );

			if ( !body ) {
				return std::unexpected( std::move( body ).error( ) );
			}

			auto page = parse_page( *body );

			if ( !page ) {
				return std::unexpected( std::move( page ).error( ) );
			}

			for ( auto& tool : page->tools ) {
				all.push_back( std::move( tool ) );
			}

			if ( page->next_cursor.empty( ) ) {
				break;
			}

			cursor = std::move( page->next_cursor );
		}

		return all;
	}

	auto client::call_tool( const std::string_view tool_name,
		const std::string_view arguments_json,
		const std::chrono::milliseconds timeout ) -> result< call_outcome > {
		auto params = std::string{ R"({"name":)" };
		params += json_escape( tool_name );
		params += R"(,"arguments":)";
		params += arguments_json.empty( ) ? std::string{ "{}" } : std::string{ arguments_json };
		params += "}";

		auto body = call( jsonrpc::TOOLS_CALL, params, timeout );

		if ( !body ) {
			return std::unexpected( std::move( body ).error( ) );
		}

		auto parsed = json::document::parse( *body );

		if ( !parsed ) {
			return std::unexpected( std::move( parsed ).error( ) );
		}

		auto outcome = call_outcome{ };
		outcome.result_json = std::move( *body );

		if ( const auto flagged = parsed->pointer_bool( "/isError" ) ) {
			outcome.is_error = *flagged;
		}

		if ( const auto text = flatten_content( outcome.result_json ) ) {
			outcome.content = std::move( *text );
		}

		return outcome;
	}

}
