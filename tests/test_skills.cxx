#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/instruct/chain.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/skills/discovery.hxx"
#include "mcode/skills/index.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto write_file( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto out = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		out << text;
	}

	auto skill_md( const std::string_view name, const std::string_view description,
		const std::string_view body = "Do the thing." ) -> std::string {
		auto out = std::string{ "---\nname: " };
		out += name;
		out += "\ndescription: \"";
		out += description;
		out += "\"\n---\n\n";
		out += body;

		return out;
	}

	auto options_with( const std::filesystem::path& workspace ) -> skills::discovery_options {
		auto options = skills::discovery_options{ };
		options.workspace = workspace;

		return options;
	}

	auto scratch( const std::string_view prefix ) -> std::filesystem::path {
		return test::scratch_directory( prefix );
	}

	auto make_registry_with_skill_read( ) -> tool_registry {
		auto registry = tool_registry{ };

		auto definition = tool_def{ };
		definition.name = "skill_read";
		definition.description = "reads a skill body";
		definition.source = tool_source::bundled_extension;
		definition.owner = "skills";
		definition.schema_json = R"({"type":"object"})";

		std::ignore = registry.add( std::move( definition ) );

		return registry;
	}

} // namespace

TEST_CASE( "frontmatter parses scalars and bools", "[skills]" ) {
	const auto text = std::string{
		"---\n"
		"name: demo-skill\n"
		"description: \"Does one thing\"\n"
		"disable-model-invocation: true\n"
		"license: MIT\n"
		"---\n\nbody\n" };

	const auto parsed = skills::parse_frontmatter( text );

	REQUIRE( parsed.has_value( ) );
	CHECK( parsed->name == "demo-skill" );
	CHECK( parsed->description == "Does one thing" );
	CHECK( parsed->disable_model_invocation );
	CHECK( skills::frontmatter_span( text ).value( ) == text.size( ) - 6 );
}

TEST_CASE( "frontmatter refuses structures the subset does not support", "[skills]" ) {
	const auto anchored = std::string{
		"---\n"
		"name: base\n"
		"description: &anchor value\n"
		"---\n" };

	CHECK( !skills::parse_frontmatter( anchored ).has_value( ) );

	const auto nested = std::string{
		"---\n"
		"name: base\n"
		"metadata:\n"
		"  author: someone\n"
		"---\n" };

	CHECK( !skills::parse_frontmatter( nested ).has_value( ) );

	const auto listed = std::string{
		"---\n"
		"name: base\n"
		"tags:\n"
		"  - one\n"
		"---\n" };

	CHECK( !skills::parse_frontmatter( listed ).has_value( ) );

	const auto unquoted_flow = std::string{
		"---\n"
		"name: base\n"
		"description: [one, two]\n"
		"---\n" };

	CHECK( !skills::parse_frontmatter( unquoted_flow ).has_value( ) );

	const auto unterminated = std::string{ "---\nname: base\n" };

	CHECK( !skills::parse_frontmatter( unterminated ).has_value( ) );

	const auto absent = std::string{ "just markdown\n" };

	CHECK( !skills::parse_frontmatter( absent ).has_value( ) );

	const auto missing_description = std::string{ "---\nname: base\n---\n" };

	CHECK( !skills::parse_frontmatter( missing_description ).has_value( ) );

	const auto bad_flag = std::string{
		"---\n"
		"name: base\n"
		"description: x\n"
		"disable-model-invocation: yes\n"
		"---\n" };

	CHECK( !skills::parse_frontmatter( bad_flag ).has_value( ) );
}

TEST_CASE( "a skill on disk is discovered, indexed and readable", "[skills]" ) {
	const auto root = scratch( "skills-basic" );
	const auto directory = root / ".mcode" / "skills" / "demo";

	write_file( directory / "SKILL.md", skill_md( "demo", "Deploy the service" ) );

	const auto report = skills::discover_skills( options_with( root ) );

	REQUIRE( report.rejected.empty( ) );
	REQUIRE( report.entries.size( ) == 1 );
	CHECK( report.entries.front( ).name == "demo" );
	CHECK( report.entries.front( ).origin == skills::skill_origin::project );

	const auto body = skills::read_skill_body( report.entries, "demo" );

	REQUIRE( body.has_value( ) );
	CHECK( body->find( "Deploy the service" ) == std::string::npos );
	CHECK( body->find( "Do the thing." ) != std::string::npos );
}

TEST_CASE( "an invalid skill is skipped, not fatal", "[skills]" ) {
	const auto root = scratch( "skills-invalid" );

	write_file( root / ".mcode" / "skills" / "demo" / "SKILL.md",
		skill_md( "demo", "valid" ) );
	write_file( root / ".mcode" / "skills" / "other" / "SKILL.md",
		skill_md( "mismatch", "name does not match the directory" ) );

	const auto report = skills::discover_skills( options_with( root ) );

	REQUIRE( report.entries.size( ) == 1 );
	CHECK( report.entries.front( ).name == "demo" );
	REQUIRE( report.rejected.size( ) == 1 );
	CHECK( report.rejected.front( ).find( "other" ) != std::string::npos );
}

TEST_CASE( "collision precedence is project over user over extension", "[skills]" ) {
	const auto root = scratch( "skills-collision" );
	const auto user_root = scratch( "skills-collision-user" );
	const auto extension_root = scratch( "skills-collision-ext" );

	write_file( root / ".mcode" / "skills" / "deploy" / "SKILL.md",
		skill_md( "deploy", "project copy" ) );
	write_file( user_root / "deploy" / "SKILL.md", skill_md( "deploy", "user copy" ) );
	write_file( extension_root / "skills" / "deploy" / "SKILL.md",
		skill_md( "deploy", "extension copy" ) );

	auto options = options_with( root );
	options.user_root = user_root;
	options.extension_roots.push_back( extension_root );

	const auto report = skills::discover_skills( options );

	REQUIRE( report.entries.size( ) == 1 );
	CHECK( report.entries.front( ).origin == skills::skill_origin::project );
	CHECK( report.entries.front( ).description == "project copy" );
}

TEST_CASE( "a symlinked skill root does not double-index", "[skills]" ) {
	const auto root = scratch( "skills-symlink" );
	const auto real_root = scratch( "skills-symlink-real" );

	write_file( real_root / "real" / "SKILL.md", skill_md( "real", "the only copy" ) );

	auto link = root / "link";
	auto error = std::error_code{ };

	std::filesystem::create_directories( root );
	std::filesystem::create_directory_symlink( real_root, link, error );

	if ( error ) {
		// a symlink needs privileges on some Windows setups; without one the test is vacuous.
		WARN( "skipped: cannot create a symlink here (" << error.message( ) << ")" );

		return;
	}

	auto options = options_with( root );
	options.user_root = real_root;

	const auto report = skills::discover_skills( options );

	REQUIRE( report.entries.size( ) == 1 );
	CHECK( report.entries.front( ).name == "real" );
}

TEST_CASE( "the user-global root is discovered for a repo without the skill", "[skills]" ) {
	const auto root = scratch( "skills-user-repo" );
	const auto user_root = scratch( "skills-user-global" );

	write_file( user_root / "global-skill" / "SKILL.md",
		skill_md( "global-skill", "lives outside the repository" ) );

	auto options = options_with( root );
	options.user_root = user_root;

	const auto report = skills::discover_skills( options );

	REQUIRE( report.entries.size( ) == 1 );
	CHECK( report.entries.front( ).name == "global-skill" );
	CHECK( report.entries.front( ).origin == skills::skill_origin::user );
}

TEST_CASE( "disable-model-invocation hides a skill from the index but keeps it listed",
	"[skills]" ) {
	const auto root = scratch( "skills-hidden" );

	write_file( root / ".mcode" / "skills" / "manual" / "SKILL.md",
		"---\nname: manual\ndescription: \"Side-effectful\"\n"
		"disable-model-invocation: true\n---\n\nbody\n" );
	write_file( root / ".mcode" / "skills" / "auto" / "SKILL.md",
		skill_md( "auto", " routable" ) );

	const auto report = skills::discover_skills( options_with( root ) );

	REQUIRE( report.entries.size( ) == 2 );

	const auto index = skills::render_index( report.entries );

	CHECK( index.find( "auto" ) != std::string::npos );
	CHECK( index.find( "manual" ) == std::string::npos );

	const auto lines = skills::index_lines( report.entries );

	REQUIRE( lines.size( ) == 2 );

	const auto manual = std::find_if( lines.begin( ), lines.end( ),
		[ ]( const skills::index_line& line ) { return line.name == "manual"; } );

	REQUIRE( manual != lines.end( ) );
	CHECK( manual->hidden );
}

TEST_CASE( "descriptions are sanitized before they reach the prompt", "[skills]" ) {
	const auto cleaned = skills::sanitize_description( "a<b>&c" "\x01" "d" "\x7F" );

	CHECK( cleaned == "a&lt;b&gt;&amp;cd" );
}

TEST_CASE( "the index renders one line per visible skill", "[skills]" ) {
	const auto root = scratch( "skills-index" );

	write_file( root / ".mcode" / "skills" / "alpha" / "SKILL.md",
		skill_md( "alpha", "first" ) );
	write_file( root / ".mcode" / "skills" / "beta" / "SKILL.md",
		skill_md( "beta", std::string( 400, 'x' ) ) );

	const auto report = skills::discover_skills( options_with( root ) );
	const auto index = skills::render_index( report.entries );

	auto line_count = std::size_t{ 0 };

	for ( const auto character : index ) {
		if ( character == '\n' ) {
			++line_count;
		}
	}

	CHECK( line_count == 2 );
	CHECK( index.starts_with( "alpha — first\n" ) );
	CHECK( index.find( "beta — " ) != std::string::npos );
	CHECK( index.size( ) < 500 );
}

TEST_CASE( "the skills extension registers skill_read and serves bodies", "[skills]" ) {
	const auto root = scratch( "skills-extension" );

	write_file( root / ".mcode" / "skills" / "demo" / "SKILL.md",
		skill_md( "demo", "a real body", "step one, step two" ) );

	const auto report = skills::discover_skills( options_with( root ) );
	REQUIRE( report.entries.size( ) == 1 );

	auto registry = tool_registry{ };
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = [ &report, &registry ]( const ext::registration& given )
		-> status {
		return given.surface.install( ext::api_surface::install_request{
			.host = given.host, .registry = registry, .providers = given.providers,
			.hooks = given.hooks, .details = given.details, .skills = &report } );
	};

	auto loaded = ext::load_extensions(
		{ std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks, options );

	REQUIRE( loaded.report.failed.empty( ) );

	const auto* definition = registry.find( "skill_read" );

	REQUIRE( definition != nullptr );

	auto body = loaded.invoke( "skill_read", R"({"name":"demo"})" );

	REQUIRE( body.has_value( ) );
	CHECK( body->find( "step one, step two" ) != std::string::npos );

	auto missing = loaded.invoke( "skill_read", R"({"name":"nope"})" );

	REQUIRE( !missing.has_value( ) );
	CHECK( missing.error( ).msg.find( "no skill with that name" ) != std::string::npos );

	auto without_name = loaded.invoke( "skill_read", R"({})" );

	CHECK( !without_name.has_value( ) );
}

TEST_CASE( "the skill index is emitted only when skill_read is registered", "[skills]" ) {
	const auto root = scratch( "skills-conditional" );

	write_file( root / ".mcode" / "skills" / "demo" / "SKILL.md",
		skill_md( "demo", "routable skill" ) );

	const auto report = skills::discover_skills( options_with( root ) );
	const auto index = skills::render_index( report.entries );

	REQUIRE( !index.empty( ) );

	auto with_tool = make_registry_with_skill_read( );
	auto without_tool = tool_registry{ };

	const auto with = build_system_prompt( with_tool, std::string{ }, index );

	CHECK( with.find( "demo" ) != std::string::npos );
	CHECK( with.find( "skill_read" ) != std::string::npos );

	const auto without = build_system_prompt( without_tool, std::string{ }, index );

	// a prompt naming a tool the model does not have is an unsatisfiable rule.
	CHECK( without.find( "demo" ) == std::string::npos );
	CHECK( without.find( "skill_read" ) == std::string::npos );
	CHECK( without.find( "# Skills" ) == std::string::npos );
}

TEST_CASE( "the session-start budget holds with a realistic chain and 15 skills",
	"[skills]" ) {
	const auto root = scratch( "skills-budget" );

	for ( auto index = 1; index <= 15; ++index ) {
		const auto name = "skill-number-" + std::to_string( index );

		write_file( root / ".mcode" / "skills" / name / "SKILL.md",
			skill_md( name, "Uses when the task touches area number "
				+ std::to_string( index )
				+ "; covers the procedure, its preconditions and its verification" ) );
	}

	auto chain = instruct::chain_options{ };
	chain.start = root;

	const auto assembled = instruct::assemble_chain( chain );

	REQUIRE( assembled.warnings.empty( ) );

	const auto index = skills::render_index(
		skills::discover_skills( options_with( root ) ).entries );

	REQUIRE( !index.empty( ) );

	auto registry = make_registry_with_skill_read( );

	const auto prompt = build_system_prompt( registry, assembled.text, index );

	const auto prompt_tokens = static_cast< std::int64_t >( prompt.size( )
		/ CHARS_PER_TOKEN_ESTIMATE );
	const auto chain_tokens = static_cast< std::int64_t >( assembled.text.size( )
		/ CHARS_PER_TOKEN_ESTIMATE );
	const auto index_tokens = static_cast< std::int64_t >( index.size( )
		/ CHARS_PER_TOKEN_ESTIMATE );

	CHECK( prompt_tokens <= SYSTEM_PROMPT_TOKEN_BUDGET );
	CHECK( chain_tokens <= INSTRUCTION_CHAIN_TOKEN_BUDGET );
	CHECK( index_tokens <= SKILL_INDEX_TOKEN_BUDGET );

	// all four parts: the first three alone would pass while the tools slice is over budget.
	const auto session_start_tokens = prompt_tokens + chain_tokens + index_tokens
		+ TOOLS_TOKEN_BUDGET;

	CHECK( session_start_tokens <= SESSION_START_TOKEN_BUDGET );
}
