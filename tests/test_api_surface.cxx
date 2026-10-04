// neither side is hand-copied: mcode.d.luau is parsed, the live mcode table is probed.

#include "ext_test_helpers.hxx"
#include "permission_test_helpers.hxx"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace mcode;
using namespace ext_test;

namespace {

	// grammar walked: entries at one tab (top level) or two (namespace), signatures paren-tracked.
	[[nodiscard]] auto parse_declared_entries( const std::filesystem::path& file )
		-> std::vector< std::string > {
		auto source = std::ifstream{ file, std::ios::binary };

		if ( !source ) {
			FAIL( "cannot read the definition file: " << file.string( ) );
			return { };
		}

		auto declared = std::vector< std::string >{ };
		auto in_declare = false;
		auto namespace_name = std::string{ };
		auto signature_depth = 0;

		auto line = std::string{ };

		while ( std::getline( source, line ) ) {
			if ( const auto comment = line.find( "--" ); comment != std::string::npos ) {
				line.resize( comment );
			}

			if ( line.find_first_not_of( " \t\r" ) == std::string::npos ) {
				continue;
			}

			if ( !in_declare ) {
				if ( line.starts_with( "declare mcode" ) ) {
					in_declare = true;
				}

				continue;
			}

			if ( signature_depth == 0 && namespace_name.empty( ) && line.starts_with( "}" ) ) {
				break;
			}

			const auto paren_delta = static_cast< int >( std::count( line.begin( ), line.end( ), '(' ) )
				- static_cast< int >( std::count( line.begin( ), line.end( ), ')' ) );

			if ( signature_depth > 0 ) {
				signature_depth += paren_delta;

				continue;
			}

			if ( !namespace_name.empty( ) && line.starts_with( "\t}" ) ) {
				namespace_name.clear( );

				continue;
			}

			const auto indent = line.find_first_not_of( '\t' );

			if ( indent != ( namespace_name.empty( ) ? 1 : 2 ) ) {
				continue;
			}

			const auto content = line.substr( indent );
			const auto colon = content.find( ':' );

			if ( colon == std::string::npos || colon == 0 ) {
				continue;
			}

			const auto name = content.substr( 0, colon );

			if ( name.find_first_not_of(
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" )
				!= std::string::npos ) {
				continue;
			}

			const auto after = content.find_first_not_of( " \t", colon + 1 );

			if ( after == std::string::npos ) {
				continue;
			}

			if ( content[ after ] == '(' ) {
				auto path = name;

				if ( !namespace_name.empty( ) ) {
					path = namespace_name + "." + name;
				}

				declared.push_back( path );
				signature_depth = paren_delta;
			} else if ( content[ after ] == '{' && namespace_name.empty( ) ) {
				namespace_name = name;
			}
		}

		return declared;
	}

	[[nodiscard]] auto contains( const std::vector< std::string >& present,
		const std::string_view name ) -> bool {
		for ( const auto& entry : present ) {
			if ( entry == name ) {
				return true;
			}
		}

		return false;
	}

	template< std::size_t Count >
	[[nodiscard]] auto contains( const std::array< std::string_view, Count >& present,
		const std::string_view name ) -> bool {
		for ( const auto entry : present ) {
			if ( entry == name ) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] auto missing_from( const std::vector< std::string >& expected,
		const std::vector< std::string >& present ) -> std::string {
		auto missing = std::string{ };

		for ( const auto& name : expected ) {
			if ( !contains( present, name ) ) {
				missing += " ";
				missing += name;
			}
		}

		return missing;
	}

	[[nodiscard]] auto undeclared( const std::vector< std::string >& declared,
		const std::vector< std::string >& present ) -> std::string {
		auto extra = std::string{ };

		for ( const auto& name : present ) {
			if ( !contains( declared, name ) ) {
				extra += " ";
				extra += name;
			}
		}

		return extra;
	}

}

TEST_CASE( "the declared surface and the implemented surface agree", "[surface]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	// the probe needs its own scratch root: other tests count exact loads over the shared one.
	const auto root = scratch_root( );
	const auto directory = root / "surface-probe";

	write( directory / "ext.toml",
		"name = \"surface-probe\"\nversion = \"0.1.0\"\napi_version = 1\npermissions = []\n" );

	write( directory / "init.luau", R"LUASRC(local present = {}

local function walk(prefix, container)
	for key, value in pairs(container) do
		local path = prefix == "" and key or (prefix .. "." .. key)
		local kind = type(value)

		if kind == "function" then
			table.insert(present, path)
		elseif kind == "table" then
			walk(path, value)
		end
	end
end

walk("", mcode)

mcode.tool.register({
	name = "surface_report",
	description = "Returns which mcode.* entry points are callable",
	schema = { type = "object", properties = {} },
	run = function(_args, _context)
		return table.concat(present, ","), nil
	end,
})
)LUASRC" );

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( loaded.report.failed.empty( ) );

	auto probe = loaded.invoke( "surface_report", "{}" );

	REQUIRE( static_cast< bool >( probe ) );

	if ( !probe ) {
		return;
	}

	auto present = std::vector< std::string >{ };

	auto text = *probe;
	auto current = std::string{ };

	for ( const auto character : text ) {
		if ( character == ',' ) {
			present.push_back( current );
			current.clear( );

			continue;
		}

		current += character;
	}

	if ( !current.empty( ) ) {
		present.push_back( current );
	}

	auto declared = parse_declared_entries(
		std::filesystem::path{ MCODE_EXTENSIONS_ROOT } / "mcode.d.luau" );

	REQUIRE_FALSE( declared.empty( ) );

	constexpr auto ABSENT = std::array< std::string_view, 1 >{ "spawn" };

	auto required = std::vector< std::string >{ };

	for ( const auto& name : declared ) {
		if ( !contains( ABSENT, name ) ) {
			required.push_back( name );
		}
	}

	const auto missing = missing_from( required, present );
	const auto extra = undeclared( declared, present );

	if ( !missing.empty( ) ) {
		FAIL( "declared but not implemented:" << missing );
	}

	if ( !extra.empty( ) ) {
		FAIL( "implemented but not declared:" << extra );
	}

	for ( const auto name : ABSENT ) {
		INFO( "deliberately absent: " << name );
		REQUIRE_FALSE( contains( present, name ) );
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "an extension tool's declared class decides its approval", "[surface]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "tool-classes";

	write( directory / "ext.toml",
		"name = \"tool-classes\"\nversion = \"0.1.0\"\napi_version = 1\npermissions = []\n" );

	write( directory / "init.luau", R"LUASRC(mcode.tool.register({
	name = "peeker",
	description = "declares read",
	permission = "read",
	schema = { type = "object", properties = {} },
	run = function(_args, _context) return "peeked", nil end,
})

mcode.tool.register({
	name = "runner",
	description = "declares exec",
	permission = "exec",
	schema = { type = "object", properties = {} },
	run = function(_args, _context) return "ran", nil end,
})

mcode.tool.register({
	name = "scribbler",
	description = "declares write",
	permission = "write",
	schema = { type = "object", properties = {} },
	run = function(_args, _context) return "wrote", nil end,
})

mcode.tool.register({
	name = "mystery",
	description = "declares nothing",
	schema = { type = "object", properties = {} },
	run = function(_args, _context) return "?", nil end,
})
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.loaded.size( ) == 1 );

	const auto* peeker = registry.find( "peeker" );
	const auto* runner = registry.find( "runner" );
	const auto* scribbler = registry.find( "scribbler" );
	const auto* mystery = registry.find( "mystery" );

	REQUIRE( peeker != nullptr );
	REQUIRE( runner != nullptr );
	REQUIRE( scribbler != nullptr );
	REQUIRE( mystery != nullptr );

	REQUIRE( peeker->klass == tool_class::read );
	REQUIRE( runner->klass == tool_class::exec );
	REQUIRE( scribbler->klass == tool_class::write );

	// an undeclared class must not inherit the read default, which allows inside the workspace
	REQUIRE( mystery->klass == tool_class::exec );

	auto setup = permission_test::rig{ };

	auto peek = perm::permission_request{ };
	peek.tool_name = peeker->name;
	peek.klass = peeker->klass;
	peek.resource = "notes.txt";

	REQUIRE( setup.engine.decide( peek ) == perm::permission_decision::allow );
	REQUIRE( setup.approval.asks( ) == 0 );

	// the workspace root itself is inside the boundary, so the write class is what matters
	auto scribble = perm::permission_request{ };
	scribble.tool_name = scribbler->name;
	scribble.klass = scribbler->klass;
	scribble.resource = setup.path.string( ) + "/notes.txt";

	REQUIRE( setup.engine.decide( scribble ) == perm::permission_decision::allow );
	REQUIRE( setup.approval.asks( ) == 0 );

	// an extension tool's arguments carry no `command`, so the resource is empty
	auto run = perm::permission_request{ };
	run.tool_name = runner->name;
	run.klass = runner->klass;

	REQUIRE( setup.engine.decide( run ) != perm::permission_decision::allow );
	REQUIRE( setup.approval.asks( ) == 1 );

	auto undeclared = perm::permission_request{ };
	undeclared.tool_name = mystery->name;
	undeclared.klass = mystery->klass;

	REQUIRE( setup.engine.decide( undeclared ) != perm::permission_decision::allow );
	REQUIRE( setup.approval.asks( ) == 2 );

	std::filesystem::remove_all( root );
}