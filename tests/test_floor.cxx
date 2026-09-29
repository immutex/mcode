// The hard-deny floor and its near-miss table. Split out of
// test_permissions.cxx when it passed the 600-line limit.

#include <catch2/catch_test_macros.hpp>

#include "permission_test_helpers.hxx"

namespace permission_test {

	TEST_CASE( "the hard-deny floor: denied commands and their legitimate near-misses",
		"[perm][floor]" ) {
		auto setup = rig{ true };

		// Denied: recursive delete of a root.
		CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -rf //" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -rf /./" ) == perm::permission_decision::deny );

		// Allowed: the near-misses.
		CHECK( setup.exec( "rm -rf ./build" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "rm -rf node_modules" ) == perm::permission_decision::allow );

		// Denied: recursive delete of the home.
		CHECK( setup.exec( "rm -rf ~" ) == perm::permission_decision::deny );

		// Allowed: a path UNDER the home is not the home.
		CHECK( setup.exec( "rm -rf ~/scratch/project-a" ) == perm::permission_decision::allow );

		// Denied: privilege escalation as the first token.
		CHECK( setup.exec( "sudo apt install x" ) == perm::permission_decision::deny );

		// Allowed: the word as an argument.
		CHECK( setup.exec( "echo sudo" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "grep sudo README.md" ) == perm::permission_decision::allow );

		// Denied: raw write to a device.
		CHECK( setup.exec( "dd if=/dev/zero of=/dev/sda" ) == perm::permission_decision::deny );

		// Allowed: a file target.
		CHECK( setup.exec( "dd if=/dev/zero of=./image.img bs=1M count=10" )
			== perm::permission_decision::allow );

		// Denied: filesystem creation.
		CHECK( setup.exec( "mkfs.ext4 /dev/sdb1" ) == perm::permission_decision::deny );

		// Allowed: the word as an argument to something else.
		CHECK( setup.exec( "man mkfs.ext4" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "a write to .git internals is denied; .gitignore is not", "[perm][floor]" ) {
		auto setup = rig{ };

		CHECK( setup.write( ".git/config" ) == perm::permission_decision::deny );
		CHECK( setup.write( ".mcode/config.toml" ) == perm::permission_decision::deny );

		// The near-miss: .gitignore merely starts with the same characters. A
		// prefix match here would refuse every .gitignore edit.
		CHECK( setup.write( ".gitignore" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "the floor cannot be overridden by a config rule", "[perm][floor]" ) {
		auto setup = rig{ };

		// Even a managed-scope allow for the exact argv must not lift the floor:
		// the floor is a separate check ahead of the rule merge, not a rule.
		setup.engine.add_config_rules( perm::rule_scope::managed, { }, { "rm -rf /" } );

		CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
	}

	TEST_CASE( "the protected-path floor denies .git internals and not .gitignore",
		"[perm][floor][near-miss]" ) {
		// The near-miss that matters: `is_protected` matches the `.git/` directory,
		// and `.gitignore` merely starts with the same characters. A prefix match
		// would refuse every `.gitignore` edit, a file the agent edits constantly.
		//
		// The fail-closed branch for a path that cannot be canonicalized is not
		// asserted here: on this platform `weakly_canonical` resolves every input
		// reachable from a test, so there is no input that reaches it. It is
		// defensive, and it is recorded as uncovered rather than tested with an
		// assertion that cannot fail.
		auto setup = rig{ };

		auto protected_path = perm::permission_request{ };
		protected_path.tool_name = "write";
		protected_path.klass = tool_class::write;
		protected_path.resource = ".git/config";

		CHECK( setup.engine.on_floor( protected_path ).has_value( ) );
		CHECK( setup.engine.decide( protected_path ) == perm::permission_decision::deny );

		auto ordinary = perm::permission_request{ };
		ordinary.tool_name = "write";
		ordinary.klass = tool_class::write;
		ordinary.resource = ".gitignore";

		CHECK_FALSE( setup.engine.on_floor( ordinary ).has_value( ) );
		CHECK( setup.engine.decide( ordinary ) == perm::permission_decision::allow );
	}

	TEST_CASE( "a quoted token with a space is not re-split by the floor",
		"[perm][floor][near-miss]" ) {
		// `on_floor` used to re-split the canonical argv on spaces, so one token
		// containing a space became two and the target resolved to a path nobody
		// named. The near-miss is that a genuine root delete still fires.
		auto setup = rig{ };

		auto ordinary = perm::permission_request{ };
		ordinary.tool_name = "bash";
		ordinary.klass = tool_class::exec;
		ordinary.resource = R"(rm -rf "my dir")";

		CHECK_FALSE( setup.engine.on_floor( ordinary ).has_value( ) );

		auto root_delete = perm::permission_request{ };
		root_delete.tool_name = "bash";
		root_delete.klass = tool_class::exec;
		root_delete.resource = "rm -rf /";

		CHECK( setup.engine.on_floor( root_delete ).has_value( ) );
	}

} // namespace permission_test
