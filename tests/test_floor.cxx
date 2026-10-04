#include <catch2/catch_test_macros.hpp>

#include "permission_test_helpers.hxx"

namespace permission_test {

	TEST_CASE( "the hard-deny floor: denied commands and their legitimate near-misses",
		"[perm][floor]" ) {
		auto setup = rig{ true };

		CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -rf //" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -rf /./" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "rm -rf ./build" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "rm -rf node_modules" ) == perm::permission_decision::allow );

		CHECK( setup.exec( "rm -rf ~" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "rm -rf ~/scratch/project-a" ) == perm::permission_decision::allow );

		CHECK( setup.exec( "sudo apt install x" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "echo sudo" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "grep sudo README.md" ) == perm::permission_decision::allow );

		CHECK( setup.exec( "dd if=/dev/zero of=/dev/sda" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "dd if=/dev/zero of=./image.img bs=1M count=10" )
			== perm::permission_decision::allow );

		CHECK( setup.exec( "mkfs.ext4 /dev/sdb1" ) == perm::permission_decision::deny );

		CHECK( setup.exec( "man mkfs.ext4" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "the floor sees through a path-qualified program and a split flag cluster",
		"[perm][floor][bypass]" ) {
		auto setup = rig{ true };

		// a path-qualified `rm` is the same program, so the floor must still fire.
		CHECK( setup.exec( "/bin/rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "/usr/bin/rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm.exe -rf /" ) == perm::permission_decision::deny );

		// `-r` and `-f` in any order or cluster, long forms included.
		CHECK( setup.exec( "rm -r -f /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -f -r /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -Rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -rfv /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm -r --force /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "rm --recursive --force /" ) == perm::permission_decision::deny );

		// a path-qualified `dd` writes the same device.
		CHECK( setup.exec( "/bin/dd of=/dev/sda" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "dd.exe of=\\\\.\\physicaldrive0" )
			== perm::permission_decision::deny );

		// recursion without force, or force without recursion, is not the floor's case.
		CHECK( setup.exec( "rm -r ./build" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "rm -f ./build" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "a wrapper does not hide the program from the floor or the runner deny",
		"[perm][floor][bypass]" ) {
		auto setup = rig{ true };

		// the floor strips the wrapper itself, so an `env`-prefixed rm is still a root delete.
		CHECK( setup.exec( "env rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "env HOME=/tmp rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "nice -n 10 rm -rf /" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "timeout 5 dd of=/dev/sda" ) == perm::permission_decision::deny );

		// a wrapped shell is still a shell.
		CHECK( setup.exec( "env sh -c 'rm -rf /'" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "nohup sh -c 'rm -rf /'" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "timeout 5 sh -c 'rm -rf /'" ) == perm::permission_decision::deny );

		// `find -exec` runs a program, so it is a runner and never allowlisted.
		CHECK( setup.exec( "find . -exec rm -rf / {} +" ) == perm::permission_decision::deny );
		CHECK( setup.exec( "find . -execdir rm -rf / {} +" ) == perm::permission_decision::deny );

		// a wrapped ordinary program still reaches the rule merge.
		CHECK( setup.exec( "env git status" ) == perm::permission_decision::allow );
		CHECK( setup.exec( "nice -n 10 git status" ) == perm::permission_decision::allow );
	}

	TEST_CASE( "a write to .git internals is denied; .gitignore is not", "[perm][floor]" ) {
		auto setup = rig{ };

		CHECK( setup.write( ".git/config" ) == perm::permission_decision::deny );
		CHECK( setup.write( ".mcode/config.toml" ) == perm::permission_decision::deny );

		CHECK( setup.write( ".gitignore" ) == perm::permission_decision::allow );
		CHECK( setup.approval.asks( ) == 0 );
	}

	TEST_CASE( "the floor cannot be overridden by a config rule", "[perm][floor]" ) {
		auto setup = rig{ };

		setup.engine.add_config_rules( perm::rule_scope::managed, { }, { "rm -rf /" } );

		CHECK( setup.exec( "rm -rf /" ) == perm::permission_decision::deny );
	}

	TEST_CASE( "the protected-path floor denies .git internals and not .gitignore",
		"[perm][floor][near-miss]" ) {
		// `.gitignore` shares a prefix with the protected `.git/`, so a prefix match refuses it.
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
		// `on_floor` must not re-split the canonical argv on spaces.
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
