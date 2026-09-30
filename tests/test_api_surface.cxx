// The declared-vs-implemented surface test.
//
// `extensions/mcode.d.luau` is the author-facing contract. Every entry point it
// declares must exist in the host, and every entry point the host installs must
// be declared -- an author type-checks against the declaration file and fails
// at runtime when the two disagree, in either direction.
//
// Neither side is hand-copied. The declared list is PARSED out of the
// definition file at test time, and the implemented list is enumerated by the
// probe extension walking the live `mcode` table, so a name added to one side
// without the other fails here without anyone remembering to update a
// mirror. The earlier version of this test kept a hand-written array and
// missed two real gaps (`defer`, `notify`) while reporting a name that was
// never an entry point at all (`cmd.handler` is a parameter of `on`).

#include "ext_test_helpers.hxx"

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

	// Reads the definition file and returns every callable entry point it
	// declares, as dotted paths (`tool.register`, `on`).
	//
	// The grammar this walks is the one the file actually uses: a `declare
	// mcode: { ... }` block whose entries sit at one tab (top level) or two tabs
	// (namespace members). A declaration whose signature spans lines is tracked
	// by paren balance, which is what keeps `handler` -- a parameter of `on` --
	// out of the list, and comments are stripped before counting.
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

			// The declare block ends at the closing brace in column zero.
			if ( signature_depth == 0 && namespace_name.empty( ) && line.starts_with( "}" ) ) {
				break;
			}

			const auto paren_delta = static_cast< int >( std::count( line.begin( ), line.end( ), '(' ) )
				- static_cast< int >( std::count( line.begin( ), line.end( ), ')' ) );

			if ( signature_depth > 0 ) {
				signature_depth += paren_delta;

				continue;
			}

			// A namespace closes at one tab plus `}`.
			if ( !namespace_name.empty( ) && line.starts_with( "\t}" ) ) {
				namespace_name.clear( );

				continue;
			}

			// Entries sit at exactly one tab (top level) or two (namespace member);
			// deeper indentation is signature content and never an entry.
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

	// The deliberately-absent list is a fixed array, and the caller asks
	// whether a name is in it rather than the other way round.
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

	// The probe extension lives in its own scratch root, not the shared fixture
	// root: every other test asserts exact load counts over that root, and a
	// third directory would change what they count.
	const auto root = scratch_root( );
	const auto directory = root / "surface-probe";

	write( directory / "ext.toml",
		"name = \"surface-probe\"\nversion = \"0.1.0\"\napi_version = 1\npermissions = []\n" );

	// The probe walks the live `mcode` table instead of checking a hand-written
	// list, so an entry the host installs but the definition file does not
	// declare shows up here no matter when it was added.
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

	// The probe returns a comma-joined list as the tool's raw result string; no
	// JSON wrapper sits between the VM and this test.
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

	// Deliberately absent entries, each with its reason. Removed from the
	// declaration rather than left as an aspiration, and asserted absent from
	// the host so the decision cannot silently rot either way: `spawn` is a
	// manifest PERMISSION, not a declared function, and `bash`/`cmd` cover the
	// need.
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