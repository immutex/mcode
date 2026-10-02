#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/tools/session_reads.hxx"

namespace mcode::tools {

	struct tool_context {
		workspace* space = nullptr;
		session_reads* reads = nullptr;
		perm::permission_engine* permissions = nullptr;

		// doubles as the artifact directory name under .mcode/artifacts/
		std::string run_id;

		// --json: no terminal is attached, so every ask resolves to deny
		bool headless = false;
	};

	class tool_handler_sink {
	public:
		virtual ~tool_handler_sink( ) = default;

		virtual auto add_handler( const std::string name,
			std::function< result< std::string >( std::string_view args_json ) > handler )
			-> void = 0;
	};

	class vector_sink final : public tool_handler_sink {
	public:
		using handler = std::function< result< std::string >( std::string_view args_json ) >;

		// `value`, not `handler`: naming it after the alias trips GCC -Wshadow
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

