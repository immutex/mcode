#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::eval {

	// No model, no network, no wall-clock dependence, so a failure is always a real regression.
	struct task {
		std::string id;
		std::string description;

		std::function< result< bool >( const std::filesystem::path& fixture_root ) > run;
	};

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

	[[nodiscard]] auto builtin_tasks( ) -> std::vector< task >;

	[[nodiscard]] auto run_suite( const std::filesystem::path& fixture_root,
		std::string_view suite_name = "mcode-fixtures-v1" ) -> suite_result;

	// pass@k is capability, pass^k reliability; both are reported.
	[[nodiscard]] auto pass_at_k( const std::vector< bool >& attempts ) -> double;
	[[nodiscard]] auto pass_power_k( const std::vector< bool >& attempts ) -> double;

	[[nodiscard]] auto to_jsonl( const suite_result& result ) -> std::string;

}
