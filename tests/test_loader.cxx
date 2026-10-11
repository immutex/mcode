#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace mcode;
using namespace ext_test;

namespace {

	// a bundled extension is a directory holding an `ext.toml`; counting them means adding
	// one needs no test edit, and one that fails to load shows up as a shortfall.
	[[nodiscard]] auto extension_count_in( const std::vector< std::filesystem::path >& roots )
		-> std::size_t {
		auto count = std::size_t{ 0 };

		for ( const auto& root : roots ) {
			if ( !std::filesystem::is_directory( root ) ) {
				continue;
			}

			for ( const auto& entry : std::filesystem::directory_iterator{ root } ) {
				if ( std::filesystem::exists( entry.path( ) / "ext.toml" ) ) {
					++count;
				}
			}
		}

		return count;
	}

}

TEST_CASE( "loading registers nothing when extensions are disabled", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled = true;
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	REQUIRE( report.loaded.empty( ) );
	REQUIRE( report.total_tools( ) == 0 );
	REQUIRE( registry.empty( ) );

	REQUIRE( report.disabled == 2 );
	REQUIRE( report.failed.empty( ) );
}

TEST_CASE( "a disabled-name list skips without failing", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled_names = { "hello-tool" };
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	REQUIRE( report.disabled == 1 );

	REQUIRE( report.failed.size( ) == 1 );
	REQUIRE( report.failed.front( ).name == "broken-manifest" );
}

TEST_CASE( "a bad manifest fails the extension, not the session", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	if ( report.loaded.size( ) != 1 ) {
		auto reasons = std::string{ };

		for ( const auto& failure : report.failed ) {
			reasons += "\n  failed " + failure.name + ": " + failure.reason;
		}

		for ( const auto& entry : report.loaded ) {
			reasons += "\n  loaded " + entry.name;
		}

		FAIL( "expected exactly one loaded extension; got " << report.loaded.size( ) << reasons );
	}

	REQUIRE( report.loaded.size( ) == 1 );
	REQUIRE( report.loaded.front( ).name == "hello-tool" );

	REQUIRE( report.failed.size( ) == 1 );
	REQUIRE( report.failed.front( ).name == "broken-manifest" );
	REQUIRE_FALSE( report.failed.front( ).reason.empty( ) );
}

TEST_CASE( "load order is deterministic", "[loader]" ) {
	for ( auto attempt = 0; attempt < 3; ++attempt ) {
		auto registry = tool_registry{ };
		g_registry = &registry;
		auto providers = model::provider_registry{ };
		auto bus = events::bus{ };
		auto hooks = ext::hook_registry{ bus };
		auto options = ext::loader_options{ };
		options.register_api = register_api;

		auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

		if ( report.loaded.size( ) != 1 || report.failed.size( ) != 1 ) {
			FAIL( "unexpected report:" << describe( report ) );
		}

		REQUIRE( report.loaded.size( ) == 1 );
		REQUIRE( report.loaded.front( ).name == "hello-tool" );
		REQUIRE( report.failed.size( ) == 1 );
		REQUIRE( report.failed.front( ).name == "broken-manifest" );
	}
}

TEST_CASE( "a missing root is skipped, not an error", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions(
		{ std::filesystem::temp_directory_path( ) / "mcode-no-such-root" }, providers, hooks,
		options ).report;

	REQUIRE( report.loaded.empty( ) );
	REQUIRE( report.failed.empty( ) );
	REQUIRE( report.disabled == 0 );
}

TEST_CASE( "unloading removes exactly the owner's tools", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto core = tool_def{ };
	core.name = "read";
	core.source = tool_source::core;
	REQUIRE( static_cast< bool >( registry.add( std::move( core ) ) ) );

	auto extension_tool = tool_def{ };
	extension_tool.name = "hello";
	extension_tool.source = tool_source::user_extension;
	extension_tool.owner = "hello-tool";
	REQUIRE( static_cast< bool >( registry.add( std::move( extension_tool ) ) ) );

	auto other = tool_def{ };
	other.name = "other";
	other.source = tool_source::user_extension;
	other.owner = "other-ext";
	REQUIRE( static_cast< bool >( registry.add( std::move( other ) ) ) );

	REQUIRE( registry.size( ) == 3 );

	const auto removed = ext::unload_extension( registry, "hello-tool" );

	REQUIRE( removed == 1 );
	REQUIRE( registry.size( ) == 2 );
	REQUIRE( registry.find( "hello" ) == nullptr );

	REQUIRE( registry.find( "read" ) != nullptr );
	REQUIRE( registry.find( "other" ) != nullptr );

	REQUIRE( ext::unload_extension( registry, "never-loaded" ) == 0 );
	REQUIRE( registry.size( ) == 2 );
}

TEST_CASE( "the shipped reference providers load", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options );

	if ( report.report.loaded.size( ) != extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) ) {
		FAIL( "unexpected report:" << describe( report.report ) );
	}

	REQUIRE( report.report.loaded.size( ) ==
		extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) );

	const auto providers_entry = std::find_if( report.report.loaded.begin( ),
		report.report.loaded.end( ), []( const ext::load_outcome& outcome ) {
			return outcome.name == "providers";
		} );

	REQUIRE( providers_entry != report.report.loaded.end( ) );
	REQUIRE( providers_entry->bytes_used > 0 );
}

TEST_CASE( "the providers extension declares three providers through the API", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options );

	if ( loaded.report.loaded.size( ) != extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) ) {
		FAIL( "unexpected report:" << describe( loaded.report ) );
	}

	REQUIRE( providers.size( ) == 3 );
	REQUIRE( providers.find( "openai-chat-completions" ) != nullptr );
	REQUIRE( providers.find( "anthropic-messages" ) != nullptr );

	for ( const auto* descriptor : providers.all( ) ) {
		REQUIRE_FALSE( descriptor->endpoint.empty( ) );
		REQUIRE( descriptor->endpoint.starts_with( "https://" ) );
	}
}

TEST_CASE( "disabling every extension leaves a working registry", "[loader]" ) {
	auto registry = tool_registry{ };

	for ( const auto* name : { "read", "write", "edit", "glob", "grep", "bash" } ) {
		auto core = tool_def{ };
		core.name = name;
		core.source = tool_source::core;
		REQUIRE( static_cast< bool >( registry.add( std::move( core ) ) ) );
	}

	const auto core_count = registry.size( );

	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled = true;
	options.register_api = register_api;

	auto loaded = ext::load_extensions(
		{ extensions_root( ), std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers,
		hooks, options );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.empty( ) );
	REQUIRE( loaded.extensions.empty( ) );
	REQUIRE( loaded.report.disabled ==
		extension_count_in( { extensions_root( ), std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) );

	REQUIRE( registry.size( ) == core_count );
	REQUIRE( registry.find( "hello" ) == nullptr );
	REQUIRE( registry.find( "read" ) != nullptr );

	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "hello", "{}" ) ) );
	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "web_search", "{}" ) ) );

	REQUIRE( providers.empty( ) );
}

TEST_CASE( "the surface exposes exactly the frozen fields", "[loader]" ) {
	// mcode.api_version must be a number: a number/string comparison raises instead of coercing
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.extensions.size( ) == 1 );

	if ( loaded.extensions.empty( ) ) {
		return;
	}

	auto& host = *loaded.extensions.front( ).host;

	auto name = host.eval_to_string( "mcode.ext.name" );
	REQUIRE( static_cast< bool >( name ) );
	REQUIRE( *name == "hello-tool" );

	auto version_is_number = host.eval_to_string( "type(mcode.api_version) == 'number'" );
	REQUIRE( static_cast< bool >( version_is_number ) );
	REQUIRE( *version_is_number == "true" );

	auto version = host.eval_to_string( "mcode.api_version" );
	REQUIRE( static_cast< bool >( version ) );
	REQUIRE( *version == std::to_string( ext::API_VERSION ) );

	auto compares = host.eval_to_string( "mcode.api_version >= 1" );
	REQUIRE( static_cast< bool >( compares ) );
	REQUIRE( *compares == "true" );

	auto shape = host.eval_to_string(
		"type(mcode.tool) == 'table' and type(mcode.tool.register) == 'function'" );
	REQUIRE( static_cast< bool >( shape ) );
	REQUIRE( *shape == "true" );
}

TEST_CASE( "a required module's compile error reaches the host", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "broken-module";

	write( directory / "ext.toml",
		"name = \"broken-module\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", "local value = require(\"helper\")\nreturn value\n" );
	write( directory / "helper.luau", "local x = = 1\n" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.size( ) == 1 );

	if ( loaded.report.failed.size( ) == 1 ) {
		// the compiler's line number, not the bare module path the raise would otherwise carry
		REQUIRE( loaded.report.failed.front( ).reason.find( ":1:" ) != std::string::npos );
	}

	std::filesystem::remove_all( root );
}
