#pragma once

// Shared by test_tools.cxx. Extracted when that file passed the 600-line
// limit.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "mcode/core/registry.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/tools/exec_policy.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/file_tools.hxx"
#include "mcode/tools/register.hxx"
#include "mcode/tools/search_tools.hxx"
#include "mcode/tools/session_reads.hxx"
#include "mcode/tools/tool_args.hxx"
#include "mcode/tools/truncate.hxx"

#include "test_scratch.hxx"


namespace tools_test {

	using namespace mcode;
	using namespace mcode::tools;


	inline constexpr std::size_t TOKEN_CHARS_PER_TOKEN = 4;
	inline constexpr std::size_t CORE_SCHEMA_BUDGET_TOKENS = 3000;

	struct fixture {
		std::filesystem::path path;
		workspace space;
		session_reads reads;
		exec_policy policy;
		tool_context context;

		explicit fixture( bool yolo = false )
			: space( make_space( ) ), policy( make_policy( yolo ) ) {
			path = test::scratch_directory( "mcode-tools-test" );

			write_raw( "src/main.cxx", "int main( ) {\n\treturn 0;\n}\n" );
			write_raw( "src/util.cxx", "int helper( ) {\n\treturn 1;\n}\n" );
			write_raw( "README.md", "# fixture\n\nhello\n" );
			write_raw( "notes.txt", "alpha\nbeta\ngamma\n" );

			space = workspace::open( path ).value( );
			context.space = &space;
			context.reads = &reads;
			context.policy = &policy;
			context.run_id = "test-run";
			context.headless = true;
		}

		~fixture( ) {
			auto error_code = std::error_code{ };
			std::filesystem::remove_all( path, error_code );
		}

		auto write_raw( const std::string& name, const std::string& content ) const -> void {
			const auto target = path / name;
			std::filesystem::create_directories( target.parent_path( ) );

			auto out = std::ofstream{ target, std::ios::binary };
			out << content;
		}

		auto read_raw( const std::string& name ) const -> std::string {
			auto in = std::ifstream{ path / name, std::ios::binary };
			auto text = std::string{ };
			auto line = std::string{ };

			while ( std::getline( in, line ) ) {
				text += line;
				text += '\n';
			}

			return text;
		}

		auto call( const std::string& json ) const -> std::string {
			const auto parsed = tool_args::parse( json );
			REQUIRE( parsed );

			auto mutable_context = context;
			auto result = handle_read( *parsed, mutable_context );

			if ( result ) {
				return *result;
			}

			return result.error( ).msg;
		}

	private:
		static auto make_space( ) -> workspace {
			return workspace::open( std::filesystem::current_path( ) ).value( );
		}

		static auto make_policy( bool yolo ) -> exec_policy {
			auto value = exec_policy{ };
			value.yolo = yolo;

			return value;
		}
	};

	[[nodiscard]] auto run_tool(
		const std::function< result< std::string >( const tool_args&, tool_context& ) >& handler,
		const std::string& json, fixture& setup ) -> std::string {
		auto parsed = tool_args::parse( json );
		REQUIRE( parsed );

		auto mutable_context = setup.context;
		auto result = handler( *parsed, mutable_context );

		if ( result ) {
			return *result;
		}

		return result.error( ).msg;
	}

	[[nodiscard]] auto is_error_json( const std::string& text ) -> bool {
		return text.find( "\"ok\":false" ) != std::string::npos &&
			text.find( "\"error\":" ) != std::string::npos &&
			text.find( "\"hint\":" ) != std::string::npos &&
			text.find( "\"retryable\":" ) != std::string::npos;
	}

	class stub_sink final : public tool_handler_sink {
	public:
		using handler_type = std::function< result< std::string >( std::string_view ) >;

		auto add_handler( std::string name, handler_type handler ) -> void override {
			names.push_back( name );
			handlers.push_back( std::move( handler ) );
		}

		std::vector< std::string > names;
		std::vector< handler_type > handlers;
	};

} // namespace tools_test
