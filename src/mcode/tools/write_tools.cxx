// The write tool: create and overwrite with the read-before-write invariant.
// The edit tool lives in write_tools_edit.cxx and the shared write/edit
// helpers in write_tools_common.

#include "mcode/tools/write_tools_common.hxx"

#include <fstream>

#include "mcode/tools/errors.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"

namespace mcode::tools {

	auto handle_write( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto path = args.string_field( "path" );

		if ( !path || path->empty( ) ) {
			return error_result( "missing required argument: path",
				"pass the file to create or overwrite, relative to the workspace root", false );
		}

		const auto content = args.string_field( "content" );

		if ( !content ) {
			return error_result( "missing required argument: content",
				"pass the complete new file content; an empty file is content: \"\"", false );
		}

		auto& space = *context.space;
		auto resolved = space.resolve( *path );

		if ( resolved && space.is_protected( *resolved ) ) {
			return refuse_protected( *path );
		}

		if ( const auto denied = check_write_permission( context, *path ) ) {
			return *denied;
		}

		if ( !resolved ) {
			// The engine allowed a path outside the workspace. The workspace's
			// own writer refuses it, so the write goes to the canonical path
			// directly. Create-only: an overwrite outside the workspace has no
			// read-before-write invariant behind it, so it stays refused.
			auto candidate = std::filesystem::path{ space.root( ) } /
				std::filesystem::path{ *path };
			auto canonical = platform::canonicalize( candidate );

			if ( !canonical ) {
				return error_result( "cannot resolve " + *path + ": " + resolved.error( ).msg,
					"check the path; paths are relative to the workspace root", false );
			}

			auto error_code = std::error_code{ };
			const auto exists = std::filesystem::exists(
				platform::to_extended_path( *canonical ), error_code );

			if ( error_code ) {
				return error_result( "cannot check " + *path + ": " + error_code.message( ),
					"the path resolved but its existence could not be determined", false );
			}

			if ( exists ) {
				return error_result( "refusing to overwrite " + *path,
					"the file is outside the workspace, so the read-before-write invariant "
					"cannot protect it; delete or move it first, or write a new file",
					false );
			}

			if ( !canonical->parent_path( ).empty( ) ) {
				std::filesystem::create_directories(
					platform::to_extended_path( canonical->parent_path( ) ), error_code );

				if ( error_code ) {
					return error_result( "cannot create " +
							canonical->parent_path( ).string( ) + ": " + error_code.message( ),
						"check the parent directory is creatable", false );
				}
			}

			auto output = std::ofstream{ platform::to_extended_path( *canonical ),
				std::ios::binary | std::ios::trunc };

			if ( !output ) {
				return error_result( "cannot open " + canonical->string( ),
					"check the path is a file, not a directory, and is writable", false );
			}

			output.write( content->data( ), static_cast< std::streamsize >( content->size( ) ) );
			output.close( );

			if ( !output ) {
				return error_result( "failed to write " + canonical->string( ),
					"the file was opened but the write failed; check the disk and permissions",
					false );
			}

			auto out = std::string{ "{\"ok\":true,\"path\":\"" };
			json::append_escaped( out, *path );
			out += "\",\"bytes\":" + std::to_string( content->size( ) );
			out += ",\"mode\":\"create\",\"outside_workspace\":true}";

			return out;
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( *resolved ),
			error_code );

		if ( error_code ) {
			return error_result( "cannot check " + *path + ": " + error_code.message( ),
				"the path resolved but its existence could not be determined", false );
		}

		auto receipt = std::optional< write_receipt >{ };

		if ( !exists ) {
			auto created = space.write_file( *path, *content, write_mode::create );

			if ( !created ) {
				return error_result( "write failed: " + created.error( ).msg,
					"check the parent directory exists and the path is a file, not a directory",
					false );
			}

			receipt = *created;
		} else {
			const auto invariant = check_readable_state( *context.reads, *resolved, *path );

			if ( !invariant ) {
				return std::unexpected( invariant.error( ) );
			}

			auto replaced = space.write_file( *path, *content, write_mode::overwrite );

			if ( !replaced ) {
				return error_result( "write failed: " + replaced.error( ).msg,
					"the file exists and was read; a failure here is a filesystem error, not a policy one",
					false );
			}

			receipt = *replaced;
		}

		context.reads->record( *resolved, receipt->content_hash );

		auto out = std::string{ "{\"ok\":true,\"path\":\"" };
		json::append_escaped( out, *path );
		out += "\",\"bytes\":" + std::to_string( receipt->bytes_written );
		out += ",\"mode\":\"" + std::string{ exists ? "overwrite" : "create" } + "\"}";

		return out;
	}

}
