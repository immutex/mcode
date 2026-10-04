#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/api_mcp.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/mcp_config.hxx"
#include "mcode/support/toml.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto write_file( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto out = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		out << text;
	}

	auto parse_servers( const std::string_view text )
		-> result< std::vector< mcp::server_config > > {
		auto parsed = toml::parse( text );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		return mcp::parse_mcp_servers( *parsed );
	}

	auto find_server( const std::vector< mcp::server_config >& servers,
		const std::string_view name ) -> const mcp::server_config* {
		for ( const auto& server : servers ) {
			if ( server.name == name ) {
				return &server;
			}
		}

		return nullptr;
	}

}

TEST_CASE( "a config with an mcp server loads", "[config][mcp]" ) {
	auto root = test::scratch_directory( "mcp-config-ok" );

	write_file( root / "config.toml", R"(
[mcp.servers.filesystem]
command = ["npx"]
args = ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/allow"]
enabled = true
)" );

	auto loaded = config::load_layer( config::scope::user, root / "config.toml" );

	REQUIRE( static_cast< bool >( loaded ) );

	if ( !loaded ) {
		FAIL( loaded.error( ).msg );

		return;
	}

	REQUIRE( loaded->values.contains( "mcp.servers.filesystem.command" ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "an unknown section still fails loudly", "[config][mcp]" ) {
	auto root = test::scratch_directory( "mcp-config-unknown" );

	write_file( root / "config.toml", R"(
[telemetry]
enabled = false
)" );

	auto refused = config::load_layer( config::scope::user, root / "config.toml" );

	REQUIRE_FALSE( static_cast< bool >( refused ) );

	if ( refused ) {
		return;
	}

	REQUIRE( refused.error( ).msg.find( "unknown key" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "the docs 22 example config loads verbatim", "[config][mcp]" ) {
	auto root = test::scratch_directory( "mcp-config-docs" );

	// inline tables are outside the TOML subset this build reads
	write_file( root / "config.toml", R"([model]
provider = "openai-compatible"
base_url = "https://example.com"
model    = "some-model"
api_key_env = "MCODE_API_KEY"
tier.plan = "frontier"
tier.act  = "workhorse"
tier.side  = "cheap"

[agent]
max_steps        = 100
max_tokens       = 2000000
max_budget_usd   = 5.0
compact_at_pct   = 80
clear_tools_at_pct = 60
max_parallel_tools = 8

[context]
read_lines_default = 100
tool_result_inline_tokens = 2000
instruction_chain_bytes = 32768

[sandbox]
mode     = "workspace"
approval = "on-request"
egress_allowlist = []

[ui]
show_tool_calls = true
theme           = "auto"
verbosity       = "concise"

[extensions]
enabled = true

[extensions.git]
enabled = false

[mcp.servers.filesystem]
command = ["npx"]
args = ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/allow"]
enabled = true
)" );

	auto loaded = config::load_layer( config::scope::user, root / "config.toml" );

	REQUIRE( static_cast< bool >( loaded ) );

	if ( !loaded ) {
		FAIL( loaded.error( ).msg );

		return;
	}

	auto servers = mcp::parse_mcp_servers( loaded->values );

	REQUIRE( static_cast< bool >( servers ) );

	if ( !servers ) {
		return;
	}

	REQUIRE( servers->size( ) == 1 );

	const auto* filesystem = find_server( *servers, "filesystem" );

	REQUIRE( filesystem != nullptr );

	if ( filesystem == nullptr ) {
		return;
	}

	REQUIRE( filesystem->command.size( ) == 4 );
	REQUIRE( filesystem->command.front( ) == "npx" );
	REQUIRE( filesystem->enabled );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a server without enabled = true stays disabled", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
command = ["npx"]
)" );

	REQUIRE( static_cast< bool >( servers ) );

	if ( !servers ) {
		return;
	}

	REQUIRE( servers->size( ) == 1 );
	REQUIRE( servers->front( ).name == "filesystem" );
	REQUIRE_FALSE( servers->front( ).enabled );
	REQUIRE( servers->front( ).source == mcp::server_source::config );
}

TEST_CASE( "a missing command is refused with a named error", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
enabled = true
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "'command' is required" ) != std::string::npos );
}

TEST_CASE( "a malformed command is refused", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
command = "npx"
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "'command' must be an array" ) != std::string::npos );
}

TEST_CASE( "args without a command is refused", "[mcp]" ) {
	// args alone would become argv[0], so the args would be executed as the program
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
args = ["/bin/sh", "-c", "evil"]
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "'args' requires 'command'" ) != std::string::npos );
}

TEST_CASE( "an unknown field under a server is refused", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
command = ["npx"]
env = "production"
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "no mcp server field is named" ) != std::string::npos );
}

TEST_CASE( "an unknown transport is refused", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers.filesystem]
command = ["npx"]
transport = "http"
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "transport must be" ) != std::string::npos );
}

TEST_CASE( "a server name that would collide with the prefix is refused", "[mcp]" ) {
	auto servers = parse_servers( R"(
[mcp.servers."a__b"]
command = ["npx"]
)" );

	REQUIRE_FALSE( static_cast< bool >( servers ) );

	if ( servers ) {
		return;
	}

	REQUIRE( servers.error( ).msg.find( "must not contain '__'" ) != std::string::npos );
}

TEST_CASE( "the token estimate uses the named heuristic", "[mcp]" ) {
	// the threshold is in tokens at four characters each, so crossing needs an extra token
	const auto at_threshold = std::string( 32768, 'x' );

	CHECK( mcp::estimate_schema_tokens( at_threshold ) == mcp::MCP_SCHEMA_TOKEN_WARNING );
	CHECK_FALSE( mcp::estimate_exceeds_warning(
		mcp::estimate_schema_tokens( at_threshold ) ) );

	const auto over_threshold = std::string( 32772, 'x' );

	CHECK( mcp::estimate_exceeds_warning(
		mcp::estimate_schema_tokens( over_threshold ) ) );

	const auto message = mcp::schema_warning_message( "notion", 17161 );

	CHECK( message.find( "17161" ) != std::string::npos );
	CHECK( message.find( "notion" ) != std::string::npos );
}

TEST_CASE( "mcode.mcp.register without the mcp permission returns nil, err",
	"[mcp][loader]" ) {
	const auto root = test::scratch_directory( "mcp-ext-noperm" );
	const auto directory = root / "no-mcp-perm";

	write_file( directory / "ext.toml",
		"name = \"no-mcp-perm\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write_file( directory / "init.luau", R"LUASRC(local ok, err = mcode.mcp.register({
	name = "files",
	transport = "stdio",
	command = { "server" },
})

if not ok then
	mcode.log.info("refused", err)
end
)LUASRC" );

	auto registry = tool_registry{ };
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto store = ext::mcp_server_store{ };

	auto options = ext::loader_options{ };
	options.register_api = [ &store, &registry ]( const ext::registration& given ) -> status {
		return given.surface.install( ext::api_surface::install_request{
			.host = given.host, .registry = registry, .providers = given.providers,
			.hooks = given.hooks, .details = given.details, .servers = &store } );
	};

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.failed.empty( ) );
	REQUIRE( loaded.extensions.size( ) == 1 );

	REQUIRE( store.all( ).empty( ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "mcode.mcp.register with the mcp permission produces the config shape",
	"[mcp][loader]" ) {
	const auto root = test::scratch_directory( "mcp-ext-perm" );
	const auto directory = root / "mcp-ext";

	write_file( directory / "ext.toml",
		"name = \"mcp-ext\"\nversion = \"0.1.0\"\napi_version = 1\n"
		"permissions = [\"mcp\"]\n" );
	write_file( directory / "init.luau", R"LUASRC(local ok, err = mcode.mcp.register({
	name = "files",
	transport = "stdio",
	command = { "server", "--flag" },
	tools = { "one", "two" },
})

if not ok then
	error(err)
end
)LUASRC" );

	auto registry = tool_registry{ };
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto store = ext::mcp_server_store{ };

	auto options = ext::loader_options{ };
	options.register_api = [ &store, &registry ]( const ext::registration& given ) -> status {
		return given.surface.install( ext::api_surface::install_request{
			.host = given.host, .registry = registry, .providers = given.providers,
			.hooks = given.hooks, .details = given.details, .servers = &store } );
	};

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.failed.empty( ) );

	REQUIRE( store.all( ).size( ) == 1 );

	const auto* declared = store.find( "files" );

	REQUIRE( declared != nullptr );

	if ( declared == nullptr ) {
		return;
	}

	REQUIRE( declared->transport == "stdio" );
	REQUIRE( declared->command.size( ) == 2 );
	REQUIRE( declared->command.front( ) == "server" );
	REQUIRE( declared->tools.size( ) == 2 );
	REQUIRE( declared->source == mcp::server_source::extension );

	REQUIRE_FALSE( declared->enabled );

	auto config_side = mcp::server_config{ };
	config_side.name = "files";
	config_side.transport = "stdio";
	config_side.command = { "server", "--flag" };
	config_side.tools = { "one", "two" };
	config_side.source = mcp::server_source::extension;
	config_side.enabled = false;

	REQUIRE( declared->name == config_side.name );
	REQUIRE( declared->transport == config_side.transport );
	REQUIRE( declared->command == config_side.command );
	REQUIRE( declared->tools == config_side.tools );
	REQUIRE( declared->enabled == config_side.enabled );
	REQUIRE( declared->source == config_side.source );

	std::filesystem::remove_all( root );
}

TEST_CASE( "mcode.mcp.register refuses a malformed declaration", "[mcp][loader]" ) {
	const auto root = test::scratch_directory( "mcp-ext-bad" );
	const auto directory = root / "mcp-ext-bad-cmd";

	write_file( directory / "ext.toml",
		"name = \"mcp-ext-bad-cmd\"\nversion = \"0.1.0\"\napi_version = 1\n"
		"permissions = [\"mcp\"]\n" );
	write_file( directory / "init.luau", R"LUASRC(local ok, err = mcode.mcp.register({
	name = "empty",
	transport = "stdio",
})

if not ok then
	mcode.log.info("refused", err)
end
)LUASRC" );

	auto registry = tool_registry{ };
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto store = ext::mcp_server_store{ };

	auto options = ext::loader_options{ };
	options.register_api = [ &store, &registry ]( const ext::registration& given ) -> status {
		return given.surface.install( ext::api_surface::install_request{
			.host = given.host, .registry = registry, .providers = given.providers,
			.hooks = given.hooks, .details = given.details, .servers = &store } );
	};

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.failed.empty( ) );
	REQUIRE( store.all( ).empty( ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a duplicate server name is refused through the store", "[mcp]" ) {
	auto store = ext::mcp_server_store{ };

	auto first = mcp::server_config{ };
	first.name = "files";
	first.transport = "stdio";
	first.command = { "a" };

	auto second = first;

	CHECK( static_cast< bool >( store.add( std::move( first ) ) ) );
	CHECK_FALSE( static_cast< bool >( store.add( std::move( second ) ) ) );
}
