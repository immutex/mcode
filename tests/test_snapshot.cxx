#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>

#include "mcode/fs/snapshot.hxx"

#include "loop_test_helpers.hxx"
#include "test_scratch.hxx"

using namespace mcode;

namespace {

	// A scratch workspace and a scratch store, both removed on the way out.
	struct snapshot_fixture {
		std::filesystem::path workspace;
		std::filesystem::path store;

		snapshot_fixture( ) {
			workspace = test::scratch_directory( "mcode-snapshot-ws" );
			store = test::scratch_directory( "mcode-snapshot-store" );
		}

		~snapshot_fixture( ) {
			auto error_code = std::error_code{ };
			std::filesystem::remove_all( workspace, error_code );
			std::filesystem::remove_all( store, error_code );
		}

		// The file the store is asked to capture.
		auto write( const std::string& name, const std::string& content ) const -> void {
			write_at( workspace, name, content );
		}

		// The index and the blobs live in the store, not in the workspace.
		auto write_store( const std::string& name, const std::string& content ) const -> void {
			write_at( store, name, content );
		}

		auto read( const std::string& name ) const -> std::string {
			auto in = std::ifstream{ workspace / name, std::ios::binary };

			return std::string{ std::istreambuf_iterator< char >{ in },
				std::istreambuf_iterator< char >{ } };
		}

		[[nodiscard]] auto target( const std::string& name ) const -> std::filesystem::path {
			return workspace / name;
		}

		[[nodiscard]] auto blob_count( ) const -> std::size_t {
			auto count = std::size_t{ 0 };

			for ( const auto& entry : std::filesystem::directory_iterator( store / "blobs" ) ) {
				(void)entry;
				++count;
			}

			return count;
		}

	private:
		static auto write_at( const std::filesystem::path& base, const std::string& name,
			const std::string& content ) -> void {
			auto out = std::ofstream{ base / name, std::ios::binary | std::ios::trunc };
			out << content;
		}
	};

	// The loop wiring the capture hook needs, without the network a full fixture implies.
	struct loop_fixture {
		loop_test::scripted_client client;
		tool_registry registry;
		event_log log;
		events::bus bus;
		snapshot_store store;

		agent_loop::dependencies deps;

		explicit loop_fixture( const std::filesystem::path& workspace,
			const std::filesystem::path& store_root )
			: store( store_root ) {
			deps.client = &client;
			deps.registry = &registry;
			deps.log = &log;
			deps.bus = &bus;
			deps.model_name = "test-model";
			deps.caps.context_window = loop_test::TEST_CONTEXT_WINDOW;
			deps.workspace_root = workspace.string( );
			deps.snapshots = &store;
		}
	};

}

TEST_CASE( "a capture restores the bytes an edit replaced", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "notes.txt", "original\n" );

	REQUIRE( store.capture( files.workspace, files.target( "notes.txt" ), "run-1" ) );

	files.write( "notes.txt", "replaced\n" );

	REQUIRE( files.read( "notes.txt" ) == "replaced\n" );

	const auto restored = store.restore_last( files.workspace, "run-1" );

	REQUIRE( restored );
	CHECK( *restored == 1 );
	CHECK( files.read( "notes.txt" ) == "original\n" );
}

TEST_CASE( "a file that did not exist is deleted by a restore", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	REQUIRE( store.capture( files.workspace, files.target( "new.txt" ), "run-1" ) );

	files.write( "new.txt", "created\n" );

	REQUIRE( std::filesystem::exists( files.target( "new.txt" ) ) );

	const auto restored = store.restore_last( files.workspace, "run-1" );

	REQUIRE( restored );
	CHECK( *restored == 1 );
	CHECK_FALSE( std::filesystem::exists( files.target( "new.txt" ) ) );
}

TEST_CASE( "the newest capture of a path wins", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "app.cxx", "first\n" );
	REQUIRE( store.capture( files.workspace, files.target( "app.cxx" ), "run-1" ) );

	files.write( "app.cxx", "second\n" );
	REQUIRE( store.capture( files.workspace, files.target( "app.cxx" ), "run-1" ) );

	files.write( "app.cxx", "third\n" );

	const auto restored = store.restore_last( files.workspace, "run-1" );

	REQUIRE( restored );
	CHECK( *restored == 1 );
	CHECK( files.read( "app.cxx" ) == "second\n" );
}

TEST_CASE( "restore_last touches only the run it names", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "a0\n" );
	files.write( "b.txt", "b0\n" );

	REQUIRE( store.capture( files.workspace, files.target( "a.txt" ), "run-1" ) );
	REQUIRE( store.capture( files.workspace, files.target( "b.txt" ), "run-2" ) );

	files.write( "a.txt", "a1\n" );
	files.write( "b.txt", "b1\n" );

	const auto restored = store.restore_last( files.workspace, "run-1" );

	REQUIRE( restored );
	CHECK( *restored == 1 );
	CHECK( files.read( "a.txt" ) == "a0\n" );
	CHECK( files.read( "b.txt" ) == "b1\n" );

	const auto everything = store.restore_all( files.workspace );

	REQUIRE( everything );
	CHECK( *everything == 1 );
	CHECK( files.read( "b.txt" ) == "b0\n" );
}

TEST_CASE( "a malformed index is an error, never a silent empty store", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "a0\n" );
	REQUIRE( store.capture( files.workspace, files.target( "a.txt" ), "run-1" ) );

	files.write( "a.txt", "a1\n" );

	// A torn append is the realistic corruption: the index keeps its prefix, loses its tail.
	files.write_store( "index.jsonl",
		R"({"run":"run-1","seq":0,"path":"a.txt","existed":true,"blob":"00)" );

	const auto restored = store.restore_last( files.workspace, "run-1" );

	CHECK_FALSE( restored );
	CHECK( files.read( "a.txt" ) == "a1\n" );
}

TEST_CASE( "an index entry whose blob is gone is an error", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "a0\n" );
	REQUIRE( store.capture( files.workspace, files.target( "a.txt" ), "run-1" ) );

	files.write( "a.txt", "a1\n" );

	for ( const auto& entry : std::filesystem::directory_iterator( files.store / "blobs" ) ) {
		std::filesystem::remove( entry.path( ) );
	}

	const auto restored = store.restore_last( files.workspace, "run-1" );

	CHECK_FALSE( restored );
	CHECK( files.read( "a.txt" ) == "a1\n" );
}

TEST_CASE( "a blob that does not hash to its own name is refused", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "a0\n" );
	REQUIRE( store.capture( files.workspace, files.target( "a.txt" ), "run-1" ) );

	files.write( "a.txt", "a1\n" );

	for ( const auto& entry : std::filesystem::directory_iterator( files.store / "blobs" ) ) {
		auto out = std::ofstream{ entry.path( ), std::ios::binary | std::ios::trunc };
		out << "not the bytes that named this blob\n";
	}

	const auto restored = store.restore_last( files.workspace, "run-1" );

	CHECK_FALSE( restored );
	CHECK( files.read( "a.txt" ) == "a1\n" );
}

TEST_CASE( "the oldest run is evicted past the retained-run cap", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "seed\n" );

	for ( auto index = std::size_t{ 0 }; index <= MAX_RETAINED_RUNS; ++index ) {
		REQUIRE( store.capture( files.workspace, files.target( "a.txt" ),
			"run-" + std::to_string( index ) ) );
	}

	// The oldest run's entry is gone, so it restores nothing rather than reaching for a blob
	// that was deleted along with it.
	const auto evicted = store.restore_last( files.workspace, "run-0" );

	REQUIRE( evicted );
	CHECK( *evicted == 0 );

	files.write( "a.txt", "changed\n" );

	const auto newest = store.restore_last( files.workspace,
		"run-" + std::to_string( MAX_RETAINED_RUNS ) );

	REQUIRE( newest );
	CHECK( *newest == 1 );
	CHECK( files.read( "a.txt" ) == "seed\n" );

	// Every retained capture holds the same bytes, so exactly one blob survives eviction.
	CHECK( files.blob_count( ) == 1 );
}

TEST_CASE( "a capture outside the workspace is refused", "[snapshot]" ) {
	const auto files = snapshot_fixture{ };
	auto store = snapshot_store{ files.store };

	files.write( "a.txt", "a0\n" );

	const auto outside = store.capture( files.workspace,
		files.workspace.parent_path( ) / "elsewhere.txt", "run-1" );

	CHECK_FALSE( outside );

	// A tampered index cannot point a restore above the root either.
	files.write_store( "index.jsonl",
		R"({"run":"run-1","seq":0,"path":"../escape.txt","existed":true,"blob":"deadbeef"})" "\n" );

	const auto escaped = store.restore_all( files.workspace );

	CHECK_FALSE( escaped );
}

TEST_CASE( "a write-class tool call is captured before the handler runs", "[snapshot][loop]" ) {
	const auto files = snapshot_fixture{ };
	auto fx = loop_fixture{ files.workspace, files.store };
	auto loop = agent_loop{ fx.deps };

	auto definition = tool_def{ };
	definition.name = "write";
	definition.description = "writes a file";
	definition.klass = tool_class::write;
	definition.schema_json = R"({"type":"object"})";

	std::ignore = fx.registry.add( definition );

	auto wrote = false;

	loop.register_handler( "write",
		[ & ]( const std::string_view args ) -> result< std::string > {
			wrote = true;

			auto out = std::ofstream{ files.target( "target.txt" ),
				std::ios::binary | std::ios::trunc };
			out << "edited by the tool\n";

			return std::string{ args };
		} );

	files.write( "target.txt", "before the edit\n" );

	fx.client.queue( loop_test::call_response( "write", R"({"path":"target.txt"})" ) );
	fx.client.queue( loop_test::text_response( "done" ) );

	REQUIRE( loop.run( "edit the file" ) );
	REQUIRE( wrote );

	CHECK( files.read( "target.txt" ) == "edited by the tool\n" );

	// The run id groups the capture, which is exactly what /undo restores by.
	REQUIRE_FALSE( loop.run_id( ).empty( ) );

	const auto restored = fx.store.restore_last( files.workspace, loop.run_id( ) );

	REQUIRE( restored );
	CHECK( *restored == 1 );
	CHECK( files.read( "target.txt" ) == "before the edit\n" );
}

TEST_CASE( "a read-class tool call captures nothing", "[snapshot][loop]" ) {
	const auto files = snapshot_fixture{ };
	auto fx = loop_fixture{ files.workspace, files.store };
	auto loop = agent_loop{ fx.deps };

	// Registered as read, so the call reaches the capture hook and the class guard decides.
	auto definition = tool_def{ };
	definition.name = "peek";
	definition.description = "reads a file";
	definition.klass = tool_class::read;
	definition.schema_json = R"({"type":"object"})";

	std::ignore = fx.registry.add( definition );

	loop.register_handler( "peek", []( const std::string_view args ) -> result< std::string > {
		return std::string{ args };
	} );

	fx.client.queue( loop_test::call_response( "peek", R"({"path":"target.txt"})" ) );
	fx.client.queue( loop_test::text_response( "done" ) );

	REQUIRE( loop.run( "peek at the file" ) );

	const auto restored = fx.store.restore_all( files.workspace );

	REQUIRE( restored );
	CHECK( *restored == 0 );

	// Nothing was captured, so the store was never created either.
	CHECK_FALSE( std::filesystem::exists( files.store / "index.jsonl" ) );
}
