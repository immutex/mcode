#pragma once

// Shared by test_tools.cxx. Extracted when that file passed the 600-line
// limit.

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

#include "mcode/core/registry.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/tools/errors.hxx"
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

	// A scripted approval source: answers come from a queue, never from a
	// terminal. Records what it was asked, so tests can assert prompt counts.
	class scripted_approval_source final : public perm::approval_source {
	public:
		auto queue( const perm::approval_outcome answer ) -> void {
			answers_.push_back( answer );
		}

		[[nodiscard]] auto asks( ) const noexcept -> std::size_t {
			return asks_;
		}

		[[nodiscard]] auto last_request( ) const -> const perm::approval_request& {
			return last_request_;
		}

		[[nodiscard]] auto ask( const perm::approval_request& request,
			const std::function< std::string( ) >& detail ) -> perm::approval_outcome override {
			++asks_;
			last_request_ = request;

			std::ignore = detail;

			if ( answers_.empty( ) ) {
				return perm::approval_outcome::refused;
			}

			const auto answer = answers_.front( );
			answers_.pop_front( );

			return answer;
		}

	private:
		std::deque< perm::approval_outcome > answers_;
		std::size_t asks_ = 0;
		perm::approval_request last_request_;
	};

	struct fixture {
		std::filesystem::path path;
		workspace space;
		session_reads reads;
		perm::remember_store store;
		scripted_approval_source approval;
		perm::permission_engine engine;
		tool_context context;

		explicit fixture( const bool yolo = false )
			: space( make_space( ) ),
			store( test::scratch_directory( "mcode-perm-store" ) / "permissions.json" ),
			engine( space, &store ) {
			path = test::scratch_directory( "mcode-tools-test" );

			write_raw( "src/main.cxx", "int main( ) {\n\treturn 0;\n}\n" );
			write_raw( "src/util.cxx", "int helper( ) {\n\treturn 1;\n}\n" );
			write_raw( "README.md", "# fixture\n\nhello\n" );
			write_raw( "notes.txt", "alpha\nbeta\ngamma\n" );

			space = workspace::open( path ).value( );
			engine = perm::permission_engine{ space, &store };

			auto options = perm::permission_engine::options{ };
			options.yolo = yolo;
			// The engine must be free to prompt: the scripted source answers
			// without a terminal. context.headless stays true for the ask_user
			// tests, which is a separate flag.
			options.headless = false;
			engine.set_options( options );
			engine.set_approval_source( &approval );

			context.space = &space;
			context.reads = &reads;
			context.permissions = &engine;
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
	};

	[[nodiscard]] inline auto run_tool(
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

	[[nodiscard]] inline auto is_error_json( const std::string& text ) -> bool {
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
