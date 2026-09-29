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

	// Five common servers spend 13% of a 200K window on tool schemas before the
	// first user message, with a 25x spread measured across real servers. A
	// server whose estimated schemas pass this many tokens is reported with the
	// measured cost, never silently loaded.
	inline constexpr std::int64_t MCP_SCHEMA_TOKEN_WARNING = 8192;

	// The client-side estimate is characters over this divisor, the same
	// heuristic the loop's budget accounting uses.
	inline constexpr std::int64_t MCP_CHARS_PER_TOKEN = 4;

	// Where a declaration came from. The transport treats both alike after
	// parsing; the difference is only who wrote the value down.
	enum class server_source {
		config,
		extension,
	};

	// One MCP server declaration, from `[mcp.servers.<name>]` in config.toml or
	// from `mcode.mcp.register` in an extension. Both paths funnel into this
	// struct so there is one implementation of everything after parsing.
	//
	// `command` is the full argv, program first. It is a code-execution vector
	// and is carried verbatim: the transport spawns it without a shell, and
	// nothing that handles a server ever joins it into a string a shell would
	// parse.
	struct server_config {
		std::string name;
		std::string transport;
		std::vector< std::string > command;
		std::vector< std::string > tools;
		bool enabled = false;
		server_source source = server_source::config;
	};

	// One validation for both paths. A missing or empty command is refused,
	// never defaulted; a transport this build does not speak is refused here
	// rather than failing later and less clearly; the name must stay usable
	// inside `mcp__<server>__<tool>`.
	[[nodiscard]] auto validate_server( const server_config& server ) -> status;

	// Parses every `[mcp.servers.<name>]` entry out of a config table. A server
	// without `enabled = true` stays disabled, and any key under `[mcp]` that is
	// not a server field is an error rather than an ignored setting.
	[[nodiscard]] auto parse_mcp_servers( const toml::table& values )
		-> result< std::vector< server_config > >;

	// The same parse over a merged config's flattened key map, which is a
	// `std::map`, not a `toml::table`.
	[[nodiscard]] auto parse_mcp_servers(
		const std::map< std::string, toml::value, std::less<> >& values )
		-> result< std::vector< server_config > >;

	// The schema-cost estimate for one server's tools, in tokens.
	[[nodiscard]] auto estimate_schema_tokens( const std::string_view schema_json ) noexcept
		-> std::int64_t;

	// True when an estimate crosses the warning threshold.
	[[nodiscard]] auto estimate_exceeds_warning( const std::int64_t tokens ) noexcept -> bool;

	// The warning text for a server past the threshold, carrying the measured
	// cost so the number the user reads is the number that was computed.
	[[nodiscard]] auto schema_warning_message( const std::string_view server_name,
		const std::int64_t tokens ) -> std::string;

}
