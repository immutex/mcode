#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::eval {

	// A deterministic task. No model, no network, no wall-clock dependence: each
	// task asserts a harness behaviour against the fixture repo, so a failure is
	// always a real regression and never sampling noise.
	//
	// docs/11 is explicit that small-n suites swing wildly at model pass rates.
	// These tasks sidestep that entirely by not involving a model -- which is also
	// the only honest thing M0 can do, since there is no model client yet.
	struct task {
		std::string id;
		std::string description;

		// Returns true on pass. The message explains a failure.
		std::function< result< bool >( const std::filesystem::path& fixture_root ) > run;
	};

	// Per-run record (docs/11 §Minimal eval harness design), JSONL, one object
	// per task.
	struct run_record {
		std::string run_id;
		std::string timestamp;
		std::string suite;
		std::string task_id;
		std::string scaffold_revision;

		std::string verdict;   // pass | fail | error
		int exit_code = 0;
		std::string detail;

		std::uint64_t tool_calls = 0;
		double wall_seconds = 0.0;

		[[nodiscard]] auto to_json( ) const -> std::string;
	};

	struct suite_result {
		std::string suite;
		std::string run_id;

		std::vector< run_record > records;

		std::size_t passed = 0;
		std::size_t failed = 0;
		std::size_t errored = 0;

		[[nodiscard]] auto total( ) const noexcept -> std::size_t {
			return records.size( );
		}

		[[nodiscard]] auto all_passed( ) const noexcept -> bool {
			return failed == 0 && errored == 0 && !records.empty( );
		}
	};

	// The ten deterministic tasks. Each names the harness surface it covers.
	[[nodiscard]] auto builtin_tasks( ) -> std::vector< task >;

	// Runs every task and returns the records.
	[[nodiscard]] auto run_suite( const std::filesystem::path& fixture_root,
		std::string_view suite_name = "mcode-fixtures-v1" ) -> suite_result;

	// pass@k: did ANY of k attempts succeed. Capability.
	// pass^k: did ALL k attempts succeed. Reliability.
	//
	// docs/11 requires both, because reporting only pass@k flatters a harness that
	// works half the time.
	[[nodiscard]] auto pass_at_k( const std::vector< bool >& attempts ) -> double;
	[[nodiscard]] auto pass_power_k( const std::vector< bool >& attempts ) -> double;

	// Serializes a suite result as JSONL, one record per line.
	[[nodiscard]] auto to_jsonl( const suite_result& result ) -> std::string;

}
