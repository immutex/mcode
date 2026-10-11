#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/perm/argv.hxx"
#include "mcode/perm/permission.hxx"

#include "test_scratch.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;
using namespace tools_test;

TEST_CASE( "deny beats allow regardless of scope order", "[perm][engine]" ) {
	auto setup = fixture{ };

	setup.engine.add_config_rules( perm::rule_scope::user, { }, { "git status" } );
	setup.engine.add_config_rules( perm::rule_scope::session, { "git status" }, { } );

	auto request = perm::permission_request{ };
	request.tool_name = "bash";
	request.klass = tool_class::exec;
	request.resource = "git status";

	CHECK( setup.engine.decide( request ) == perm::permission_decision::deny );

	auto swapped = fixture{ };
	swapped.engine.add_config_rules( perm::rule_scope::user, { "git status" }, { } );
	swapped.engine.add_config_rules( perm::rule_scope::session, { }, { "git status" } );

	CHECK( swapped.engine.decide( request ) == perm::permission_decision::deny );
}

TEST_CASE( "a remembered allow is argv-exact", "[perm][engine]" ) {
	auto setup = fixture{ };

	setup.approval.queue( perm::approval_outcome::allow_remember );

	auto status = perm::permission_request{ };
	status.tool_name = "bash";
	status.klass = tool_class::exec;
	status.resource = "git status";

	CHECK( setup.engine.decide( status ) == perm::permission_decision::allow );

	auto push = perm::permission_request{ };
	push.tool_name = "bash";
	push.klass = tool_class::exec;
	push.resource = "git push";

	setup.approval.queue( perm::approval_outcome::deny_once );
	CHECK( setup.engine.decide( push ) == perm::permission_decision::deny );
}

TEST_CASE( "redirection and newline are refused as command shape", "[perm][argv]" ) {
	CHECK_FALSE( perm::parse_command_line( "git log > .git/hooks/pre-commit" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "sort < input.txt" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( std::string{ "echo a\nrm -rf /" } ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( std::string{ "echo a\rb" } ).has_value( ) );
}

TEST_CASE( "runner detection survives spelling differences", "[perm][argv]" ) {
	CHECK( perm::is_exec_runner( "/bin/sh" ) );
	CHECK( perm::is_exec_runner( "C:\\Windows\\System32\\cmd.exe" ) );
	CHECK( perm::is_exec_runner( "CMD" ) );
	CHECK( perm::is_exec_runner( "Cmd.exe" ) );
	CHECK( perm::is_exec_runner( "pwsh" ) );
	CHECK( perm::is_exec_runner( "bash.exe" ) );
	CHECK_FALSE( perm::is_exec_runner( "git" ) );
	CHECK_FALSE( perm::is_exec_runner( "cmake" ) );
}

TEST_CASE( "variable expansion is refused", "[perm][argv]" ) {
	CHECK_FALSE( perm::parse_command_line( "echo $HOME" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "echo ${PATH}" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "echo %APPDATA%" ).has_value( ) );
}
