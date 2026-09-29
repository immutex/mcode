#include "mcode/support/mcp_config.hxx"

#include <algorithm>
#include <array>

namespace mcode::mcp {

	namespace {

		// The section is `[mcp]` and every server lives under `mcp.servers.<name>`.
		constexpr auto SERVERS_PREFIX = std::string_view{ "mcp.servers." };

		// The fields one server accepts. Anything else is a typo, and a typo in a
		// launch command is the case that must not pass silently.
		constexpr auto SERVER_FIELDS = std::array{
			std::string_view{ "command" },
			std::string_view{ "args" },
			std::string_view{ "enabled" },
			std::string_view{ "transport" },
			std::string_view{ "tools" },
		};

		// The only transport this build speaks. A declared other one is refused
		// rather than spawned and failed later.
		constexpr auto STDIO_TRANSPORT = std::string_view{ "stdio" };

		// A server name lands inside `mcp__<server>__<tool>`, so it must be
		// non-empty and cannot carry the separator that prefixing relies on.
		auto is_valid_server_name( const std::string_view name ) -> bool {
			if ( name.empty( ) ) {
				return false;
			}

			return name.find( "__" ) == std::string_view::npos;
		}

		auto server_error( const std::string_view name, const std::string_view problem )
			-> error {
			return fail( errc::config,
				"mcp server '" + std::string{ name } + "': " + std::string{ problem } );
		}

	}

	auto validate_server( const server_config& server ) -> status {
		if ( !is_valid_server_name( server.name ) ) {
			return std::unexpected( fail( errc::config,
				"mcp server name must be non-empty and must not contain '__'" ) );
		}

		if ( server.transport != STDIO_TRANSPORT ) {
			return std::unexpected( server_error( server.name,
				"transport must be \"stdio\"; no other transport ships" ) );
		}

		// The launch command is the code-execution vector. An empty or missing one
		// is refused, never defaulted to something plausible.
		if ( server.command.empty( ) ) {
			return std::unexpected( server_error( server.name,
				"'command' is required and must name the server's executable" ) );
		}

		if ( server.command.front( ).empty( ) ) {
			return std::unexpected( server_error( server.name,
				"'command' must not start with an empty argument" ) );
		}

		return { };
	}

	auto parse_mcp_servers( const toml::table& values )
		-> result< std::vector< server_config > > {
		auto out = std::vector< server_config >{ };

		for ( const auto& full_key : values.keys( ) ) {
			// The map iterates key -> value; only the key drives the parse, and
			// each value is read through find( ) at its full dotted path.
			const auto& key = full_key.first;

			if ( !key.starts_with( SERVERS_PREFIX ) ) {
				continue;
			}

			// `mcp.servers.<name>` plus an optional field: the remainder after the
			// prefix is the server name, and anything past a dot inside it is the
			// field this row sets.
			const auto remainder = key.substr( SERVERS_PREFIX.size( ) );
			const auto separator = remainder.find( '.' );
			const auto server_name = remainder.substr( 0, separator );

			if ( server_name.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"'" + key + "' does not name a server under mcp.servers" ) );
			}

			if ( separator != std::string_view::npos ) {
				const auto field = remainder.substr( separator + 1 );

				if ( std::find( SERVER_FIELDS.begin( ), SERVER_FIELDS.end( ), field )
					== SERVER_FIELDS.end( ) ) {
					return std::unexpected( fail( errc::config,
						"unknown key '" + key + "'; no mcp server field is named '" +
						std::string{ field } + "'" ) );
				}

				continue;
			}

			// A bare `mcp.servers.<name>` row is the section header itself; the
			// fields live at `mcp.servers.<name>.<field>` and are read below.
			auto server = server_config{ };
			server.name = std::string{ server_name };
			server.transport = STDIO_TRANSPORT;

			// A newly configured server is disabled unless the user wrote
			// `enabled = true`. Enabling is explicit; absence is not consent.

			if ( const auto* command = values.find( std::string{ key } + ".command" ) ) {
				auto argv = command->as_string_array( );

				if ( !argv ) {
					return std::unexpected( fail( errc::config,
						"mcp server '" + std::string{ server_name } +
						"': 'command' must be an array of strings" ) );
				}

				server.command = std::move( *argv );
			}

			if ( const auto* args = values.find( std::string{ key } + ".args" ) ) {
				auto extra = args->as_string_array( );

				if ( !extra ) {
					return std::unexpected( fail( errc::config,
						"mcp server '" + std::string{ server_name } +
						"': 'args' must be an array of strings" ) );
				}

				for ( auto&& item : *extra ) {
					server.command.push_back( std::move( item ) );
				}
			}

			if ( const auto* tools = values.find( std::string{ key } + ".tools" ) ) {
				auto allow = tools->as_string_array( );

				if ( !allow ) {
					return std::unexpected( fail( errc::config,
						"mcp server '" + std::string{ server_name } +
						"': 'tools' must be an array of strings" ) );
				}

				server.tools = std::move( *allow );
			}

			if ( const auto* enabled = values.find( std::string{ key } + ".enabled" ) ) {
				auto flag = enabled->as_bool( );

				if ( !flag ) {
					return std::unexpected( fail( errc::config,
						"mcp server '" + std::string{ server_name } +
						"': 'enabled' must be a boolean" ) );
				}

				server.enabled = *flag;
			}

			if ( const auto validated = validate_server( server ); !validated ) {
				return std::unexpected( validated.error( ) );
			}

			out.push_back( std::move( server ) );
		}

		return out;
	}

	auto estimate_schema_tokens( const std::string_view schema_json ) noexcept
		-> std::int64_t {
		return static_cast< std::int64_t >( schema_json.size( )
			/ static_cast< std::size_t >( MCP_CHARS_PER_TOKEN ) );
	}

	auto estimate_exceeds_warning( const std::int64_t tokens ) noexcept -> bool {
		return tokens > MCP_SCHEMA_TOKEN_WARNING;
	}

	auto schema_warning_message( const std::string_view server_name,
		const std::int64_t tokens ) -> std::string {
		return "mcp server '" + std::string{ server_name } +
			"' advertises about " + std::to_string( tokens ) +
			" tokens of tool schemas; the warning threshold is " +
			std::to_string( MCP_SCHEMA_TOKEN_WARNING ) +
			" tokens, and five common servers spend 13% of a 200K window this way";
	}

}
