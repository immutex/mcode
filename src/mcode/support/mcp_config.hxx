#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::mcp {

	// A server past this many estimated schema tokens is reported, never loaded.
	inline constexpr std::int64_t MCP_SCHEMA_TOKEN_WARNING = 8192;

	// characters per token, the same heuristic the loop's budget accounting uses.
	inline constexpr std::int64_t MCP_CHARS_PER_TOKEN = 4;

	enum class server_source {
		config,
		extension,
	};

	// `command` is the full argv: a code-execution vector, spawned without a shell.
	struct server_config {
		std::string name;
		std::string transport;
		std::vector< std::string > command;
		std::vector< std::string > tools;
		bool enabled = false;
		server_source source = server_source::config;
	};

	[[nodiscard]] auto validate_server( const server_config& server ) -> status;

	// a server without `enabled = true` stays disabled; an unknown `[mcp]` key is an error.
	[[nodiscard]] auto parse_mcp_servers( const toml::table& values )
		-> result< std::vector< server_config > >;

	[[nodiscard]] auto parse_mcp_servers(
		const std::map< std::string, toml::value, std::less<> >& values )
		-> result< std::vector< server_config > >;

	[[nodiscard]] auto estimate_schema_tokens( const std::string_view schema_json ) noexcept
		-> std::int64_t;

	[[nodiscard]] auto estimate_exceeds_warning( const std::int64_t tokens ) noexcept -> bool;

	[[nodiscard]] auto schema_warning_message( const std::string_view server_name,
		const std::int64_t tokens ) -> std::string;

}
