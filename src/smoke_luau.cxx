#include <cstdio>
#include <string>
#include <vector>

#include "mcode/ext/lua_host.hxx"

#include "smoke.hxx"

auto smoke_luau( ) -> void {
smoke::section( "Luau (extension layer)" );

{
	auto host = mcode::lua_host::create( mcode::lua_host_options{ .extension_name = "smoke", .module_loader = { } } );
	smoke::check( static_cast< bool >( host ), "created a lua_State" );

	if ( host ) {
		std::printf( "  version: %s\n", host->version_string( ).c_str( ) );

		auto registered = host->register_host_function(
			"host_ping", []( const std::string_view args ) -> mcode::result< std::string > {
				return std::string{ "pong:" } + std::string{ args };
			} );
		smoke::check( static_cast< bool >( registered ), "registered a host function into mcode.*" );

		auto pinged = host->eval_to_string( "mcode.host_ping('hello')" );
		smoke::check( pinged && *pinged == "pong:hello", "Luau called back into C++" );

		auto ran = host->run( "x = 1 + 1" );
		smoke::check( static_cast< bool >( ran ), "executed a Luau chunk" );

		auto value = host->eval_to_string( "x * 21" );
		smoke::check( value && *value == "42", "evaluated Luau and read the result" );

		smoke::check( host->sealed( ), "the API surface was sealed before extension code ran" );

		auto late = host->register_host_function( "too_late", []( const std::string_view )
			-> mcode::result< std::string > { return std::string{ }; } );
		smoke::check( !late, "registration after sealing is refused" );

		auto broken = host->run( "this is not luau" );
		smoke::check( !broken, "compile error returned as a value" );

		auto threw = host->run( "error('boom')" );
		smoke::check( !threw, "runtime error returned as a value" );

		smoke::section( "Luau boundary (every escape must fail)" );

		// Probes evaluate to "true" when the escape succeeded; a probe that cannot run fails.
		const struct {
			const char* expression;
			const char* label;
		} ESCAPES[] = {
			{ "(io ~= nil)", "io is absent" },
			{ "(package ~= nil)", "package is absent" },
			{ "(type(os.execute) ~= 'nil')", "os.execute is absent" },
			{ "(type(os.getenv) ~= 'nil')", "os.getenv is absent" },
			{ "(type(os.remove) ~= 'nil')", "os.remove is absent" },
			{ "(type(os.exit) ~= 'nil')", "os.exit is absent" },
			{ "(type(loadstring) ~= 'nil')", "loadstring is absent" },
			{ "(type(load) ~= 'nil')", "load is absent" },
			{ "(type(dofile) ~= 'nil')", "dofile is absent" },
			{ "(type(loadfile) ~= 'nil')", "loadfile is absent" },
			{ "(type(string.dump) ~= 'nil')", "string.dump is absent" },
			{ "(type(debug.getregistry) ~= 'nil')", "debug.getregistry is absent" },
			{ "(type(debug.setmetatable) ~= 'nil')", "debug.setmetatable is absent" },
			{ "(type(debug.sethook) ~= 'nil')", "debug.sethook is absent" },
			{ "(type(debug.getupvalue) ~= 'nil')", "debug.getupvalue is absent" },
			{ "(type(collectgarbage) ~= 'nil')", "collectgarbage is absent" },
			{ "select(1, pcall(rawset, _G, 'injected', 1))", "rawset on _G is refused" },
			{ "select(1, pcall(setmetatable, _G, {}))", "setmetatable on _G is refused" },
			{ "select(1, pcall(rawset, string, 'injected', 1))", "rawset on a library table is refused" },
			{ "select(1, pcall(rawset, mcode, 'injected', 1))", "rawset on the host API table is refused" },
			{ "select(1, pcall(setmetatable, '', { __index = function() return 1 end }))",
				"the string metatable is readonly" },
			{ "(type(mcode.not_registered) ~= 'nil')", "unregistered host functions are absent" },
			{ "(function() local host = getmetatable(getfenv(0)).__index "
			  "return tostring(select(1, pcall(rawset, host, 'injected', 1))) end)()",
				"the frozen host globals stay readonly when reached through getfenv" },
			{ "(function() setfenv(1, setmetatable({}, { __index = _G })) "
			  "return tostring(select(1, pcall(rawset, _G, 'injected', 1))) end)()",
				"replacing the environment with setfenv does not unfreeze _G" },
		};

		for ( const auto& probe : ESCAPES ) {
			auto outcome = host->eval_to_string( probe.expression );

			if ( !outcome ) {
				smoke::check( false, probe.label, "probe could not run: " + outcome.error( ).msg );

				continue;
			}

			smoke::check( *outcome == "false", probe.label, "escape succeeded: " + *outcome );
		}

		// Lua 5.1 shims Luau keeps: not escalations, but their presence is recorded.
		const struct {
			const char* name;
			const char* label;
		} SHIMS[] = {
			{ "newproxy", "newproxy is present (creates a tagged userdata with no host reach)" },
			{ "setfenv", "setfenv is present (changes only the caller's own environment)" },
			{ "getfenv", "getfenv is present (returns the extension's own globals proxy)" },
			{ "require", "require is present (host-injected, extension-root only)" },
		};

		for ( const auto& shim : SHIMS ) {
			auto kind = host->eval_to_string( "tostring(type(" + std::string{ shim.name } + "))" );

			smoke::check( kind && *kind == "function", shim.label,
				kind ? *kind : std::string{ "probe failed" } );
		}

		// A capability, not an absence: it must refuse anything the host did not hand it.
		auto require_refuses = host->eval_to_string(
			"tostring(select(1, pcall(require, '../../../etc/passwd')))" );
		smoke::check( require_refuses && *require_refuses == "false",
			"require refuses a path outside the extension root",
			require_refuses ? *require_refuses : std::string{ "probe failed" } );

		auto require_unknown = host->eval_to_string(
			"tostring(select(1, pcall(require, './lib/not_registered')))" );
		smoke::check( require_unknown && *require_unknown == "false",
			"require refuses a module the host never registered",
			require_unknown ? *require_unknown : std::string{ "probe failed" } );

		auto own_globals = host->eval_to_string(
			"(function() local ok = pcall(rawset, getfenv(0), 'own_global', 1) "
			"return tostring(ok and type(getfenv(0).own_global) == 'number') end)()" );
		smoke::check( own_globals && *own_globals == "true",
			"getfenv(0) is the extension's own writable namespace, not the host's",
			own_globals ? *own_globals : std::string{ "probe failed" } );

		auto proxy_safe = host->eval_to_string(
			"tostring(select(1, pcall(function() local p = newproxy(true) return p end)))" );
		smoke::check( proxy_safe && *proxy_safe == "true", "newproxy cannot be used to escape",
			proxy_safe ? *proxy_safe : std::string{ "probe failed" } );
	}
}
}
