#include "mcode/mcp/connect.hxx"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "mcode/mcp/client.hxx"
#include "mcode/mcp/constants.hxx"
#include "mcode/mcp/source.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::mcp {

	namespace {

		auto warn_stderr( const std::string& message ) -> void {
			std::fprintf( stderr, "mcode: %s\n", message.c_str( ) );
		}

		auto warn_schema_cost( const server_config& server,
			const std::vector< server_tool >& tools ) -> void {
			auto total = std::int64_t{ 0 };

			for ( const auto& tool : tools ) {
				total += estimate_schema_tokens( tool.schema_json );
			}

			if ( estimate_exceeds_warning( total ) ) {
				warn_stderr( schema_warning_message( server.name, total ) );
			}
		}

		// the supervisor is owned by the caller's server_set, which outlives the loop
		auto make_handler( supervisor& board, const std::string& server_tool_name )
			-> std::function< result< std::string >( std::string_view ) > {
			return [ &board, server_tool_name ]( const std::string_view arguments_json )
				-> result< std::string > {
				if ( board.client_ptr( ) == nullptr || !board.client_ptr( )->is_alive( ) ) {
					return std::unexpected( fail( errc::io,
						"mcp server '" + board.config( ).name + "' is not running" ) );
				}

				auto outcome = board.client_ptr( )->call_tool( server_tool_name,
					arguments_json, DEFAULT_CALL_TIMEOUT );

				if ( !outcome ) {
					return std::unexpected( outcome.error( ) );
				}

				if ( outcome->is_error ) {
					return std::unexpected( fail( errc::tool_failed,
						wrap_untrusted( outcome->content ) ) );
				}

				// wrapped once, here: call_tool stays raw for callers that need the verbatim body
				return wrap_untrusted( outcome->content );
			};
		}

		// empty on success; a disabled server is a no-op that also returns empty
		auto connect_one( const server_config& server, const connect_input& input )
			-> std::string {
			if ( !server.enabled ) {
				return { };
			}

			auto board = std::make_unique< supervisor >( server,
				supervisor::handlers{ } );

			auto listed = board->start( );

			if ( !listed ) {
				const auto message = "mcp server '" + server.name
					+ "' failed to start: " + listed.error( ).msg;

				warn_stderr( message );

				return message;
			}

			warn_schema_cost( server, *listed );

			auto registrar = source{ *input.registry };

			if ( const auto registered = registrar.register_server( server.name,
				*listed ); !registered ) {
				const auto message = "mcp server '" + server.name
					+ "' tool registration failed: " + registered.error( ).msg;

				warn_stderr( message );

				return message;
			}

			for ( const auto& tool : *listed ) {
				input.loop->register_handler(
					qualified_tool_name( server.name, tool.name ),
					make_handler( *board, tool.name ) );
			}

			input.owned->add( std::move( board ) );

			return { };
		}

	}

	auto connect_list( const std::vector< server_config >& servers,
		const connect_input& input ) -> result< connect_report > {
		auto report = connect_report{ };

		for ( const auto& server : servers ) {
			const auto problem = connect_one( server, input );

			if ( problem.empty( ) ) {
				if ( server.enabled ) {
					report.started.push_back( server.name );
				}
			} else {
				report.failed.push_back( server.name );
			}
		}

		return report;
	}

	auto connect_servers( const connect_input& input ) -> result< connect_report > {
		auto declared = std::vector< server_config >{ };

		if ( input.config_values != nullptr ) {
			auto parsed = parse_mcp_servers( *input.config_values );

			if ( !parsed ) {
				return std::unexpected( parsed.error( ) );
			}

			declared = std::move( *parsed );
		}

		if ( input.extension_servers != nullptr ) {
			for ( const auto& server : input.extension_servers->all( ) ) {
				const auto duplicate = std::any_of( declared.begin( ), declared.end( ),
					[ & ]( const server_config& existing ) {
						return existing.name == server.name;
					} );

				if ( duplicate ) {
					return std::unexpected( fail( errc::config,
						"an mcp server named '" + server.name
						+ "' is declared by both config and an extension" ) );
				}

				declared.push_back( server );
			}
		}

		return connect_list( declared, input );
	}

	auto server_set::add( std::unique_ptr< supervisor > board ) -> void {
		boards_.push_back( std::move( board ) );
	}

	auto server_set::shutdown_all( ) -> void {
		for ( auto& board : boards_ ) {
			board->shutdown( );
		}
	}

}
