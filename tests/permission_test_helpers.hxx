#pragma once

// Shared by the permission test files. Extracted when test_permissions.cxx
// passed the 600-line limit. The scripted source is deliberately separate
// from tools_test_helpers' scripted_approval_source: this header must not
// pull in the tool layer.

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "mcode/fs/workspace.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace permission_test {

	// A scripted approval source: answers come from a queue, never from a
	// terminal. Records what it was asked and the detail text it was shown,
	// so tests can assert prompt counts and the detail view.
	class scripted_source final : public perm::approval_source {
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

		auto ask( const perm::approval_request& request,
			const std::function< std::string( ) >& detail ) -> perm::approval_outcome override {
			++asks_;
			last_request_ = request;
			last_detail_ = detail( );

			if ( answers_.empty( ) ) {
				return perm::approval_outcome::refused;
			}

			const auto answer = answers_.front( );
			answers_.pop_front( );

			return answer;
		}

		[[nodiscard]] auto last_detail( ) const -> const std::string& {
			return last_detail_;
		}

	private:
		std::deque< perm::approval_outcome > answers_;
		std::size_t asks_ = 0;
		perm::approval_request last_request_;
		std::string last_detail_;
	};

	struct rig {
		std::filesystem::path path;
		workspace space;
		perm::remember_store store;
		scripted_source approval;
		perm::permission_engine engine;

		explicit rig( const bool yolo = false, const std::string approval_mode = "on-request" )
			: space( workspace::open( make_root( ) ).value( ) ),
			store( test::scratch_directory( "mcode-perm" ) / "permissions.json" ),
			engine( space, &store ) {
			path = space.root( );

			std::filesystem::create_directories( path / "src" );

			auto options = perm::permission_engine::options{ };
			options.yolo = yolo;
			options.approval = approval_mode;
			engine.set_options( options );
			engine.set_approval_source( &approval );
		}

		[[nodiscard]] static auto make_root( ) -> std::filesystem::path {
			return test::scratch_directory( "mcode-perm-ws" );
		}

		auto exec( const std::string& argv ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "bash";
			request.klass = tool_class::exec;
			request.resource = argv;

			return engine.decide( request );
		}

		auto read( const std::string& path_text ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "read";
			request.klass = tool_class::read;
			request.resource = path_text;

			return engine.decide( request );
		}

		auto write( const std::string& path_text ) -> perm::permission_decision {
			auto request = perm::permission_request{ };
			request.tool_name = "write";
			request.klass = tool_class::write;
			request.resource = path_text;

			return engine.decide( request );
		}

		// An MCP tool: its resource is an opaque arguments blob, so the store
		// must key it by name rather than by what it was called with.
		auto mcp( const std::string& tool, const std::string& arguments ) {
			auto request = perm::permission_request{ };
			request.tool_name = tool;
			request.klass = tool_class::mcp;
			request.owner = "mcp:echo";
			request.resource = arguments;

			return engine.decide( request );
		}
	};

	// Builds a store file with the given JSON body, for the project-store tests.
	inline auto write_store_file( const std::filesystem::path& file, const std::string& body ) -> void {
		std::filesystem::create_directories( file.parent_path( ) );

		auto out = std::ofstream{ file, std::ios::binary };
		out << body;
	}

} // namespace permission_test
