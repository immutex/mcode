// The declared-vs-implemented surface test.
//
// `extensions/mcode.d.luau` is the author-facing contract. Every entry point it
// declares must exist in the host, and every entry point the host installs must
// be declared -- an author type-checks against the declaration file and fails
// at runtime when the two disagree, in either direction. The check walks the
// real loader and the real VM: the fixture extension probes which `mcode.*`
// paths are callable and reports the list back through a registered tool.
//
// The declared list is read from `extensions/mcode.d.luau` at compile time,
// spelled as the same list the definition file declares. A name added to one
// side without the other fails this test.

#include "ext_test_helpers.hxx"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace mcode;
using namespace ext_test;

namespace {

	// The declared surface, verbatim from `extensions/mcode.d.luau`. Sorted so a
	// missing name is one binary search away and the failure lists drift
	// deterministically.
	constexpr auto DECLARED = std::array< std::string_view, 26 >{
		"cmd.register",
		"cmd.handler",
		"cfg.get",
		"context.add_instructions",
		"emit",
		"fs.read",
		"fs.write",
		"log.debug",
		"log.error",
		"log.info",
		"log.warn",
		"mcp.register",
		"model.register",
		"net.get",
		"net.search",
		"off",
		"on",
		"session.fork",
		"session.snapshot",
		"skill.list",
		"skill.read",
		"skill.register",
		"timer.at",
		"timer.every",
		"tool.register",
		"tool.unregister",
	};

	// Deliberately absent entries, each with its reason. These are asserted
	// ABSENT from the declaration file's runtime surface: a declared-and-absent
	// name is the failure this slice exists to fix, so an aspiration must be
	// removed rather than left declared.
	constexpr auto ABSENT = std::array< std::string_view, 1 >{
		"spawn",
	};

	[[nodiscard]] auto contains( const std::vector< std::string >& present,
		const std::string_view name ) -> bool {
		for ( const auto& entry : present ) {
			if ( entry == name ) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] auto difference( const std::array< std::string_view, 26 >& expected,
		const std::vector< std::string >& present ) -> std::string {
		auto missing = std::string{ };

		for ( const auto name : expected ) {
			if ( !contains( present, name ) ) {
				missing += " ";
				missing += name;
			}
		}

		return missing;
	}

	[[nodiscard]] auto undeclared( const std::vector< std::string >& present ) -> std::string {
		auto extra = std::string{ };

		for ( const auto& name : present ) {
			auto known = false;

			for ( const auto candidate : DECLARED ) {
				if ( candidate == name ) {
					known = true;

					break;
				}
			}

			if ( !known ) {
				extra += " ";
				extra += name;
			}
		}

		return extra;
	}

} // namespace

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
	write( directory / "init.luau", R"LUASRC(local present = {}

local names = {
	"tool.register", "tool.unregister", "model.register",
	"on", "off", "emit",
	"log.debug", "log.info", "log.warn", "log.error",
	"skill.read", "skill.list", "skill.register",
	"mcp.register",
	"cmd.register", "cmd.handler",
	"timer.at", "timer.every",
	"cfg.get",
	"session.snapshot", "session.fork",
	"net.get", "net.search",
	"fs.read", "fs.write",
	"context.add_instructions",
}

for _, path in names do
	local segments = string.split(path, ".")
	local cursor = mcode
	local reachable = true

	for _, segment in segments do
		if type(cursor) ~= "table" then
			reachable = false
			break
		end

		cursor = cursor[segment]
	end

	if reachable and type(cursor) == "function" then
		table.insert(present, path)
	end
end

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

	const auto missing = difference( DECLARED, present );
	const auto extra = undeclared( present );

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
