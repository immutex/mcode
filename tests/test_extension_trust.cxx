#include "ext_test_helpers.hxx"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/ext/integrity.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/ext/trust_store.hxx"

using namespace mcode;
using namespace ext_test;

namespace {

	constexpr auto MANIFEST = std::string_view{ R"(name = "backdoor"
version = "0.1.0"
api_version = 1
description = "a repository extension"
)" };

	constexpr auto ENTRY = std::string_view{ R"(mcode.tool.register({
	name = "backdoor_tool",
	description = "does nothing",
	schema = { type = "object" },
	run = function(args) return "ok", nil end,
})
)" };

	struct repo_fixture {
		std::filesystem::path workspace;
		std::filesystem::path root;
		std::filesystem::path extension;

		explicit repo_fixture( const std::string_view name ) {
			workspace = scratch_root( ) / name;
			std::filesystem::remove_all( workspace );

			root = ext::project_extension_root( workspace );
			extension = root / "backdoor";

			write( extension / "ext.toml", MANIFEST );
			write( extension / "init.luau", ENTRY );
		}

		~repo_fixture( ) {
			auto ignored = std::error_code{ };
			std::filesystem::remove_all( workspace, ignored );
		}

		[[nodiscard]] auto digest( ) const -> std::string {
			const auto snapshot = ext::snapshot_extension( extension );

			REQUIRE( snapshot.has_value( ) );

			return snapshot->tree_sha256;
		}

		auto store( ) const -> ext::trust_store {
			auto loaded = ext::trust_store::load( workspace / "trust" );

			REQUIRE( loaded.has_value( ) );

			return std::move( *loaded );
		}

		auto grant( const ext::trust_store& into ) const -> ext::trust_store {
			auto copy = into;

			auto record = ext::trust_record{ };
			record.scope = ext::trust_scope::project;
			record.name = "backdoor";
			record.api_version = 1;
			record.tree_sha256 = digest( );
			record.root = extension;

			REQUIRE( copy.grant( record ).has_value( ) );

			return copy;
		}
	};

	auto load_project( repo_fixture& fixture, const ext::trust_store* store,
		ext::load_result& out ) -> void {
		auto registry = tool_registry{ };
		g_registry = &registry;
		auto providers = model::provider_registry{ };
		auto bus = events::bus{ };
		auto hooks = ext::hook_registry{ bus };

		auto options = ext::loader_options{ };
		options.project_root = fixture.root;
		options.trust = store;
		options.register_api = register_api;

		out = ext::load_extensions( { fixture.root }, providers, hooks, options );
	}

}

TEST_CASE( "a repository extension with no grant is inert, not executed", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-ungranted" };
	auto loaded = ext::load_result{ };

	load_project( fixture, nullptr, loaded );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.inert.size( ) == 1 );
	REQUIRE( loaded.report.inert.front( ).name == "backdoor" );
	REQUIRE( loaded.report.inert.front( ).reason == ext::load_trust::ungranted );
	REQUIRE( loaded.tool_owners.empty( ) );
}

TEST_CASE( "a granted repository extension loads", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-granted" };
	const auto store = fixture.grant( fixture.store( ) );
	auto loaded = ext::load_result{ };

	load_project( fixture, &store, loaded );

	INFO( describe( loaded.report ) );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( loaded.report.loaded.front( ).name == "backdoor" );
	REQUIRE( loaded.report.loaded.front( ).trust == ext::load_trust::granted );
	REQUIRE( loaded.report.loaded.front( ).tree_sha256 == fixture.digest( ) );
	REQUIRE( loaded.tool_owners.contains( "backdoor_tool" ) );
}

TEST_CASE( "a changed extension is inert and the grant is not rewritten", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-changed" };
	const auto store = fixture.grant( fixture.store( ) );

	// one byte appended, so the grant stops describing the tree.
	write( fixture.extension / "init.luau", std::string{ ENTRY } + "-- appended\n" );

	auto loaded = ext::load_result{ };

	load_project( fixture, &store, loaded );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.inert.size( ) == 1 );
	REQUIRE( loaded.report.inert.front( ).reason == ext::load_trust::changed );

	const auto* record = store.record_for( ext::trust_scope::project, "backdoor" );

	REQUIRE( record != nullptr );
	REQUIRE( record->tree_sha256 != fixture.digest( ) );
}

TEST_CASE( "a denied extension is inert even with a matching digest", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-denied" };
	auto store = fixture.grant( fixture.store( ) );

	auto record = ext::trust_record{ };
	record.scope = ext::trust_scope::project;
	record.name = "backdoor";
	record.tree_sha256 = "-";
	record.root = fixture.extension;

	REQUIRE( store.grant( record ).has_value( ) );

	auto loaded = ext::load_result{ };

	load_project( fixture, &store, loaded );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.inert.size( ) == 1 );
	REQUIRE( loaded.report.inert.front( ).reason == ext::load_trust::denied );
}

TEST_CASE( "an api_version change invalidates the grant", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-api-version" };
	const auto store = fixture.grant( fixture.store( ) );

	// the loader cannot express this case: a too-high api_version is a load failure,
	// not an invalidated grant.
	REQUIRE( store.decide( ext::trust_scope::project, "backdoor", 1 )
		== ext::trust_decision::granted );

	// The digest still matches, but the surface moved under the grant.
	REQUIRE( store.decide( ext::trust_scope::project, "backdoor", 2 )
		== ext::trust_decision::ungranted );

	// A different extension is not covered by this grant at all.
	REQUIRE( store.decide( ext::trust_scope::project, "other", 1 )
		== ext::trust_decision::ungranted );
	REQUIRE( store.decide( ext::trust_scope::user, "backdoor", 1 )
		== ext::trust_decision::ungranted );
}

TEST_CASE( "an extension outside the project root needs no grant", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-user-scope" };
	auto loaded = ext::load_result{ };

	// The same directory, loaded as a root the user chose rather than the repo's.
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	loaded = ext::load_extensions( { fixture.root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( loaded.report.loaded.front( ).trust == ext::load_trust::trusted_by_location );
}

TEST_CASE( "the tree digest covers contents and membership", "[integrity]" ) {
	auto fixture = repo_fixture{ "integrity-digest" };
	const auto before = fixture.digest( );

	write( fixture.extension / "extra.luau", "return {}\n" );

	const auto after_file = fixture.digest( );
	REQUIRE( after_file != before );

	write( fixture.extension / "extra.luau", "return { x = 1 }\n" );

	REQUIRE( fixture.digest( ) != after_file );
	REQUIRE( fixture.digest( ).size( ) == 64 );
}

TEST_CASE( "a symlink anywhere in the tree is refused", "[integrity]" ) {
	auto fixture = repo_fixture{ "integrity-symlink" };
	const auto outside = fixture.workspace / "outside.luau";

	write( outside, "return 1\n" );

	auto error = std::error_code{ };
	std::filesystem::create_symlink( outside, fixture.extension / "linked.luau", error );

	if ( error ) {
		SUCCEED( "symlink creation is not permitted here" );

		return;
	}

	const auto snapshot = ext::snapshot_extension( fixture.extension );

	REQUIRE( !snapshot.has_value( ) );
	REQUIRE( snapshot.error( ).msg.find( "symlink" ) != std::string::npos );
}

TEST_CASE( "resolve_module handles the three spellings and refuses escapes", "[integrity]" ) {
	auto fixture = repo_fixture{ "integrity-resolve" };

	write( fixture.extension / "lib" / "helper.luau", "return 1\n" );

	const auto snapshot = ext::snapshot_extension( fixture.extension );
	REQUIRE( snapshot.has_value( ) );

	REQUIRE( ext::resolve_module( *snapshot, "init.luau" ) != nullptr );
	REQUIRE( ext::resolve_module( *snapshot, "lib/helper" ) != nullptr );
	REQUIRE( ext::resolve_module( *snapshot, "lib/helper.luau" ) != nullptr );

	REQUIRE( ext::resolve_module( *snapshot, "../outside" ) == nullptr );
	REQUIRE( ext::resolve_module( *snapshot, "/etc/passwd" ) == nullptr );
	REQUIRE( ext::resolve_module( *snapshot, "missing" ) == nullptr );
}

TEST_CASE( "the trust store round-trips through a file", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-store-roundtrip" };
	const auto path = fixture.workspace / "trust";

	// grant() already writes through to disk, so this only checks the reload.
	const auto store = fixture.grant( fixture.store( ) );

	REQUIRE( store.record_for( ext::trust_scope::project, "backdoor" ) != nullptr );

	auto reloaded = ext::trust_store::load( path );
	REQUIRE( reloaded.has_value( ) );

	const auto* record = reloaded->record_for( ext::trust_scope::project, "backdoor" );

	REQUIRE( record != nullptr );
	REQUIRE( record->tree_sha256 == fixture.digest( ) );
	REQUIRE( record->api_version == 1 );
}

TEST_CASE( "a deny survives a reload and does not lose the grants beside it", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-deny-roundtrip" };
	const auto path = fixture.workspace / "trust";

	// Exactly what `mcode ext deny` writes: a deny names an extension, not a
	// location, so it carries no root.
	auto store = fixture.grant( fixture.store( ) );

	auto deny = ext::trust_record{ };
	deny.scope = ext::trust_scope::project;
	deny.name = "backdoor";
	deny.tree_sha256 = "-";

	REQUIRE( store.grant( deny ).has_value( ) );

	auto reloaded = ext::trust_store::load( path );
	REQUIRE( reloaded.has_value( ) );

	REQUIRE( reloaded->decide( ext::trust_scope::project, "backdoor", 1 )
		== ext::trust_decision::denied );

	// A second extension's grant is unaffected by the deny written next to it.
	auto other = ext::trust_record{ };
	other.scope = ext::trust_scope::project;
	other.name = "neighbour";
	other.api_version = 1;
	other.tree_sha256 = fixture.digest( );
	other.root = fixture.extension;

	REQUIRE( store.grant( other ).has_value( ) );

	auto after = ext::trust_store::load( path );
	REQUIRE( after.has_value( ) );
	REQUIRE( after->decide( ext::trust_scope::project, "neighbour", 1 )
		== ext::trust_decision::granted );
	REQUIRE( after->decide( ext::trust_scope::project, "backdoor", 1 )
		== ext::trust_decision::denied );
}

TEST_CASE( "one corrupt line fails the whole trust store", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-corrupt" };
	const auto path = fixture.workspace / "trust";

	write( path, "v1\tallow\tproject\tbackdoor\t1\t" + fixture.digest( ) + "\t"
		+ fixture.extension.generic_string( ) + "\nnot a record\n" );

	// Fails closed: a store that cannot be read must not read as "nothing granted"
	// with the valid line silently honoured.
	const auto loaded = ext::trust_store::load( path );

	REQUIRE( !loaded.has_value( ) );
}

TEST_CASE( "an unknown format version is a hard error", "[trust]" ) {
	auto fixture = repo_fixture{ "trust-version" };
	const auto path = fixture.workspace / "trust";

	write( path, "v2\tallow\tproject\tbackdoor\t1\t" + fixture.digest( ) + "\t"
		+ fixture.extension.generic_string( ) + "\n" );

	REQUIRE( !ext::trust_store::load( path ).has_value( ) );
}
