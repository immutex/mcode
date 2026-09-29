#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/registry.hxx"
#include "mcode/core/error.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/tools/exec_policy.hxx"
#include "mcode/tools/session_reads.hxx"

namespace mcode::tools {

	// Everything a tool handler needs, as one request struct rather than six
	// positional parameters. The registry stays name-and-schema only; this is what
	// the handlers close over.
	struct tool_context {
		workspace* space = nullptr;
		session_reads* reads = nullptr;
		const exec_policy* policy = nullptr;

		// Run-scoped artifact directory name under `.mcode/artifacts/`, taken from
		// the event log so two runs cannot collide.
		std::string run_id;

		// The approval policy's yolo decision, made once at startup.
		bool headless = false;
	};

	// One method, matching agent_loop::register_handler's signature exactly, so
	// the loop implements it with a one-line change and tests pass a stub. The
	// tools layer never includes agent/loop.hxx -- layers point down only.
	class tool_handler_sink {
	public:
		virtual ~tool_handler_sink( ) = default;

		virtual auto add_handler( const std::string name,
			std::function< result< std::string >( std::string_view args_json ) > handler )
			-> void = 0;
	};

	// A sink that collects handlers for the caller to install. main.cxx holds the
	// registrations until the loop exists; tests replay them onto their own
	// dispatchers.
	class vector_sink final : public tool_handler_sink {
	public:
		using handler = std::function< result< std::string >( std::string_view args_json ) >;

		// The parameter is `value`, not `handler`: a parameter named after the
		// alias it carries is rejected by GCC as shadowing a member (-Wshadow),
		// and clang does not implement that warning, so the local gate missed it.
		auto add_handler( std::string name, handler value ) -> void override {
			handlers_.push_back( { std::move( name ), std::move( value ) } );
		}

		[[nodiscard]] auto take( ) -> std::vector< std::pair< std::string, handler > > {
			return std::move( handlers_ );
		}

	private:
		std::vector< std::pair< std::string, handler > > handlers_;
	};

}
