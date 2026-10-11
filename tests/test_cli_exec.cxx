#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include "mcode/cli/exec.hxx"
#include "mcode/cli/setup.hxx"
#include "mcode/core/error.hxx"

using namespace mcode;

TEST_CASE( "every exit code is the documented number", "[cli]" ) {
	// these numbers are an interface a CI script branches on.
	REQUIRE( cli::to_int( cli::exit_code::success ) == 0 );
	REQUIRE( cli::to_int( cli::exit_code::verification_failed ) == 1 );
	REQUIRE( cli::to_int( cli::exit_code::usage_error ) == 2 );
	REQUIRE( cli::to_int( cli::exit_code::budget_exhausted ) == 3 );
	REQUIRE( cli::to_int( cli::exit_code::provider_error ) == 4 );
	REQUIRE( cli::to_int( cli::exit_code::permission_denied ) == 5 );
	REQUIRE( cli::to_int( cli::exit_code::interrupted ) == 130 );
}

TEST_CASE( "budget exhaustion is not an interruption", "[cli]" ) {
	REQUIRE( cli::exit_code_for( errc::budget_exhausted )
		== cli::exit_code::budget_exhausted );
	REQUIRE( cli::exit_code_for( errc::cancelled ) == cli::exit_code::interrupted );
	REQUIRE( cli::exit_code_for( errc::budget_exhausted )
		!= cli::exit_code_for( errc::cancelled ) );
}

TEST_CASE( "the error-to-exit mapping is total", "[cli]" ) {
	// the mapping must be total: an unmapped error silently becomes 1, "verification failed".
	const auto codes = std::array{
		errc::ok, errc::io, errc::json, errc::protocol, errc::tool_failed,
		errc::cancelled, errc::budget_exhausted, errc::lua_error, errc::config,
		errc::unsupported,
	};

	for ( const auto code : codes ) {
		REQUIRE_FALSE( to_string( code ).empty( ) );
	}

	for ( const auto code : codes ) {
		if ( code == errc::ok ) {
			continue;
		}

		REQUIRE( cli::exit_code_for( code ) != cli::exit_code::success );
	}
}

TEST_CASE( "a handoff that is neither a budget stop nor a denial still fails", "[cli]" ) {
	auto completion = turn_outcome{ };
	completion.final_state = loop_state::handoff;

	// An unverified turn ends in handoff with no recorded reason: that is a completion.
	REQUIRE( cli::exit_code_for_run( completion, false, false ) == cli::exit_code::success );

	// A failed model call or a guard trip hands off with the reason recorded.
	auto gave_up = completion;
	gave_up.summary_json = R"({"reason":"thrash beyond replan guard"})";

	REQUIRE( cli::exit_code_for_run( gave_up, false, false ) == cli::exit_code::provider_error );

	// The budget and the denial stay more specific than the give-up reason.
	REQUIRE( cli::exit_code_for_run( gave_up, true, false )
		== cli::exit_code::budget_exhausted );
	REQUIRE( cli::exit_code_for_run( gave_up, false, true )
		== cli::exit_code::permission_denied );

	auto failed = turn_outcome{ };
	failed.final_state = loop_state::failed;

	REQUIRE( cli::exit_code_for_run( failed, false, false ) == cli::exit_code::provider_error );

	REQUIRE( cli::exit_code_for_run( failed, true, false )
		== cli::exit_code::budget_exhausted );

	auto done = turn_outcome{ };
	done.final_state = loop_state::done;

	REQUIRE( cli::exit_code_for_run( done, false, false ) == cli::exit_code::success );
}

TEST_CASE( "the worktree flag takes an optional name and never swallows the next flag",
	"[cli][worktree]" ) {
	// No name: the next token is a flag, so it must not be consumed as a branch label.
	auto bare = cli::parse_exec_options( { "--worktree", "--json" } );
	REQUIRE( static_cast< bool >( bare ) );
	CHECK( bare->worktree );
	CHECK( bare->worktree_name.empty( ) );
	CHECK( bare->json );

	auto named = cli::parse_exec_options( { "-w", "feature-x" } );
	REQUIRE( static_cast< bool >( named ) );
	CHECK( named->worktree );
	CHECK( named->worktree_name == "feature-x" );

	auto long_named = cli::parse_exec_options( { "--worktree", "probe" } );
	REQUIRE( static_cast< bool >( long_named ) );
	CHECK( long_named->worktree_name == "probe" );

	auto plain = cli::parse_exec_options( { "do the thing" } );
	REQUIRE( static_cast< bool >( plain ) );
	CHECK_FALSE( plain->worktree );
}

TEST_CASE( "setup replaces the model section and leaves the rest of the config alone",
	"[cli][setup]" ) {
	const auto provider = "openai-chat-completions";
	const auto model = "gpt-5";
	const auto base_url = "https://api.openai.com/v1/chat/completions";
	const auto key_env = "OPENAI_API_KEY";

	SECTION( "a CRLF config keeps one model section, not two" ) {
		const auto existing =
			"# note\r\n\r\n[model]\r\nprovider = \"old\"\r\nmodel = \"old-model\"\r\n"
			"\r\n[models.\"x\"]\r\nprice_input = 1.0\r\n";

		const auto updated = cli::splice_model_section( existing, provider, model,
			base_url, key_env );

		CHECK( updated.find( "model = \"old-model\"" ) == std::string::npos );
		CHECK( updated.find( "provider = \"old\"" ) == std::string::npos );

		auto count = std::size_t{ 0 };
		auto position = updated.find( "[model]" );

		while ( position != std::string::npos ) {
			++count;
			position = updated.find( "[model]", position + 1 );
		}

		CHECK( count == 1 );

		CHECK( updated.find( "[models.\"x\"]" ) != std::string::npos );
		CHECK( updated.find( "price_input = 1.0" ) != std::string::npos );
		CHECK( updated.find( "# note" ) != std::string::npos );

		CHECK( updated.find( "\r\n" ) != std::string::npos );

		CHECK( updated.find( "model = \"gpt-5\"" ) != std::string::npos );
		CHECK( updated.find( "api_key_env = \"OPENAI_API_KEY\"" ) != std::string::npos );
	}

	SECTION( "a header with a trailing comment is still the same section" ) {
		for ( const auto* header : { "[model] # my provider", "[model] ", "  [model]" } ) {
			const auto existing = std::string{ "# note\n" } + header
				+ "\nprovider = \"old\"\nmodel = \"old-model\"\n";

			const auto updated = cli::splice_model_section( existing, provider, model,
				base_url, key_env );

			INFO( "header: " << header );

			auto count = std::size_t{ 0 };
			auto position = updated.find( "[model]" );

			while ( position != std::string::npos ) {
				++count;
				position = updated.find( "[model]", position + 1 );
			}

			CHECK( count == 1 );
			CHECK( updated.find( "model = \"old-model\"" ) == std::string::npos );
			CHECK( updated.find( "# note" ) != std::string::npos );
			CHECK( updated.find( "model = \"gpt-5\"" ) != std::string::npos );
		}
	}

	SECTION( "an LF config is replaced in place too" ) {
		const auto existing =
			"[model]\nprovider = \"old\"\nmodel = \"old-model\"\n\n[ui]\ntheme = \"dark\"\n";

		const auto updated = cli::splice_model_section( existing, provider, model,
			base_url, key_env );

		CHECK( updated.find( "old-model" ) == std::string::npos );
		CHECK( updated.find( "model = \"gpt-5\"" ) != std::string::npos );
		CHECK( updated.find( "[ui]" ) != std::string::npos );
		CHECK( updated.find( "theme = \"dark\"" ) != std::string::npos );
		CHECK( updated.find( "\r\n" ) == std::string::npos );
	}

	SECTION( "a config with no model section gains one" ) {
		const auto existing = "[ui]\ntheme = \"dark\"\n";

		const auto updated = cli::splice_model_section( existing, provider, model,
			base_url, key_env );

		CHECK( updated.find( "[model]" ) != std::string::npos );
		CHECK( updated.find( "model = \"gpt-5\"" ) != std::string::npos );
		CHECK( updated.find( "[ui]" ) != std::string::npos );
	}

	SECTION( "an empty config produces just the section" ) {
		const auto updated = cli::splice_model_section( "", provider, model,
			base_url, key_env );

		CHECK( updated.find( "[model]" ) != std::string::npos );
		CHECK( updated.find( "provider = \"openai-chat-completions\"" ) != std::string::npos );
	}

	SECTION( "a model id containing a quote cannot break the file" ) {
		const auto updated = cli::splice_model_section( "", provider, "we\"ird",
			base_url, key_env );

		CHECK( updated.find( "model = \"we\\\"ird\"" ) != std::string::npos );
	}

	SECTION( "a models pricing block is not mistaken for the model section" ) {
		const auto existing =
			"[models.\"combo/x\"]\nprice_input = 0.15\nprice_output = 0.6\n";

		const auto updated = cli::splice_model_section( existing, provider, model,
			base_url, key_env );

		CHECK( updated.find( "[models.\"combo/x\"]" ) != std::string::npos );
		CHECK( updated.find( "price_output = 0.6" ) != std::string::npos );
		CHECK( updated.find( "\n[model]\n" ) != std::string::npos );
	}
}
