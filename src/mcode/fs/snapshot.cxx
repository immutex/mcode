#include "mcode/fs/snapshot.hxx"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include "mcode/fs/snapshot_internal.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"

namespace mcode::snapshot_detail {

	auto content_address( const std::string_view bytes ) -> std::string {
		return hash_bytes( bytes );
	}

	auto read_whole_file( const std::filesystem::path& path ) -> result< std::string > {
		auto stream = std::ifstream{ platform::to_extended_path( path ), std::ios::binary };

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
		}

		auto out = std::string{ std::istreambuf_iterator< char >{ stream },
			std::istreambuf_iterator< char >{ } };

		if ( stream.bad( ) ) {
			return std::unexpected( fail( errc::io, "read failed on " + path.string( ) ) );
		}

		return out;
	}

	auto entry_line( const snapshot_store::index_entry& entry ) -> std::string {
		auto line = std::string{ "{\"run\":\"" };
		json::append_escaped( line, entry.run );
		line += "\",\"seq\":";
		line += std::to_string( entry.sequence );
		line += ",\"path\":\"";
		json::append_escaped( line, entry.path );
		line += "\",\"existed\":";
		line += entry.existed ? "true" : "false";
		line += ",\"blob\":\"";
		json::append_escaped( line, entry.blob );
		line += "\"}\n";

		return line;
	}

}

namespace mcode {

	[[nodiscard]] auto workspace_relative_path( const std::filesystem::path& workspace_root,
		const std::filesystem::path& file ) -> result< std::string > {
		auto error_code = std::error_code{ };
		const auto root = std::filesystem::weakly_canonical( workspace_root, error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot canonicalize the workspace root: " + error_code.message( ) ) );
		}

		const auto absolute = std::filesystem::weakly_canonical( file, error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io, "cannot canonicalize " + file.string( ) +
				": " + error_code.message( ) ) );
		}

		if ( !path_is_within( root, absolute ) ) {
			return std::unexpected( fail( errc::io,
				"path is outside the workspace: " + absolute.string( ) ) );
		}

		// `path_is_within` proved the root is a component prefix, so the remainder
		// starts exactly `root`'s component count in.
		auto path_entry = absolute.begin( );

		std::advance( path_entry,
			static_cast< std::ptrdiff_t >( std::distance( root.begin( ), root.end( ) ) ) );

		if ( path_entry == absolute.end( ) ) {
			return std::unexpected( fail( errc::io,
				"path is the workspace root, not a file: " + absolute.string( ) ) );
		}

		auto relative = std::filesystem::path{ };

		for ( ; path_entry != absolute.end( ); ++path_entry ) {
			relative /= *path_entry;
		}

		return relative.generic_string( );
	}

	auto snapshot_store::index_path( ) const -> std::filesystem::path {
		return root_ / std::string{ SNAPSHOT_INDEX_NAME };
	}

	auto snapshot_store::blob_path( const std::string_view blob ) const -> std::filesystem::path {
		return root_ / std::string{ SNAPSHOT_BLOB_DIRECTORY } / std::string{ blob };
	}

	auto snapshot_store::load_index( ) const -> result< std::vector< index_entry > > {
		auto error_code = std::error_code{ };
		const auto path = index_path( );

		if ( !std::filesystem::exists( platform::to_extended_path( path ), error_code ) ) {
			if ( error_code ) {
				return std::unexpected( fail( errc::io,
					"cannot stat the snapshot index: " + error_code.message( ) ) );
			}

			// An absent index is an empty store, which is the state before the first capture.
			return std::vector< index_entry >{ };
		}

		auto stream = std::ifstream{ platform::to_extended_path( path ), std::ios::binary };

		if ( !stream ) {
			return std::unexpected(
				fail( errc::io, "cannot read the snapshot index: " + path.string( ) ) );
		}

		auto entries = std::vector< index_entry >{ };
		auto line = std::string{ };

		while ( std::getline( stream, line ) ) {
			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.pop_back( );
			}

			if ( line.empty( ) ) {
				continue;
			}

			auto parsed = json::document::parse( line );

			if ( !parsed ) {
				return std::unexpected(
					fail( errc::json, "malformed snapshot index line: " + line ) );
			}

			auto entry = index_entry{ };
			auto run = parsed->get_string( "run" );
			auto relative = parsed->get_string( "path" );
			auto existed = parsed->pointer_bool( "/existed" );
			auto sequence = parsed->get_int( "seq" );

			if ( !run || !relative || !existed || !sequence ) {
				return std::unexpected( fail( errc::json,
					"snapshot index entry is missing a required field: " + line ) );
			}

			entry.run = *run;
			entry.path = *relative;
			entry.existed = *existed;
			entry.sequence = static_cast< std::uint64_t >( *sequence );

			if ( entry.existed ) {
				auto blob = parsed->get_string( "blob" );

				if ( !blob || blob->empty( ) ) {
					return std::unexpected( fail( errc::json,
						"snapshot index entry records a file with no blob: " + line ) );
				}

				entry.blob = *blob;
			}

			entries.push_back( std::move( entry ) );
		}

		if ( stream.bad( ) ) {
			return std::unexpected(
				fail( errc::io, "read failed on the snapshot index: " + path.string( ) ) );
		}

		return entries;
	}

	auto snapshot_store::write_blob( const std::string_view blob, const std::string_view bytes )
		-> status {
		auto error_code = std::error_code{ };
		const auto path = blob_path( blob );

		// Already stored: the name is the content, so an existing blob is the right bytes.
		if ( std::filesystem::exists( platform::to_extended_path( path ), error_code ) &&
			!error_code ) {
			return status{ };
		}

		std::filesystem::create_directories( platform::to_extended_path( path.parent_path( ) ),
			error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot create the blob directory: " + error_code.message( ) ) );
		}

		// Through a sibling temp file so a crash cannot leave a half-written blob behind.
		auto temporary = path;
		temporary += std::string{ snapshot_detail::TEMPORARY_SUFFIX };

		{
			auto out = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !out ) {
				return std::unexpected(
					fail( errc::io, "cannot write the snapshot blob: " + temporary.string( ) ) );
			}

			out.write( bytes.data( ), static_cast< std::streamsize >( bytes.size( ) ) );
			out.flush( );

			if ( !out ) {
				return std::unexpected( fail( errc::io,
					"writing the snapshot blob did not complete: " + temporary.string( ) ) );
			}
		}

		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( path ), error_code );

		if ( error_code ) {
			auto ignored = std::error_code{ };
			std::filesystem::remove( platform::to_extended_path( temporary ), ignored );

			return std::unexpected( fail( errc::io,
				"cannot store the snapshot blob: " + error_code.message( ) ) );
		}

		return status{ };
	}

	auto snapshot_store::read_blob( const std::string_view blob, std::string& out ) const -> status {
		auto error_code = std::error_code{ };
		const auto path = blob_path( blob );

		if ( !std::filesystem::exists( platform::to_extended_path( path ), error_code ) ||
			error_code ) {
			return std::unexpected( fail( errc::io,
				"the snapshot index names a missing blob: " + path.string( ) ) );
		}

		auto bytes = snapshot_detail::read_whole_file( path );

		if ( !bytes ) {
			return std::unexpected( bytes.error( ) );
		}

		// The check that makes a weak name-only hash safe: bytes that no longer hash to their
		// own name are corruption, and restoring them would silently write the wrong file.
		if ( snapshot_detail::content_address( *bytes ) != blob ) {
			return std::unexpected( fail( errc::io,
				"snapshot blob does not match its content address: " + path.string( ) ) );
		}

		out = std::move( *bytes );

		return status{ };
	}

	auto snapshot_store::append_entry( const index_entry& entry ) -> status {
		auto error_code = std::error_code{ };
		std::filesystem::create_directories( platform::to_extended_path( root_ ), error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot create the snapshot directory: " + error_code.message( ) ) );
		}

		auto out = std::ofstream{ platform::to_extended_path( index_path( ) ),
			std::ios::binary | std::ios::app };

		if ( !out ) {
			return std::unexpected( fail( errc::io, "cannot open the snapshot index for append" ) );
		}

		auto line = snapshot_detail::entry_line( entry );

		out.write( line.data( ), static_cast< std::streamsize >( line.size( ) ) );
		out.flush( );

		if ( !out ) {
			return std::unexpected(
				fail( errc::io, "writing the snapshot index did not complete" ) );
		}

		return status{ };
	}

	auto snapshot_store::rewrite_index( const std::vector< index_entry >& entries ) -> status {
		auto error_code = std::error_code{ };
		std::filesystem::create_directories( platform::to_extended_path( root_ ), error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot create the snapshot directory: " + error_code.message( ) ) );
		}

		auto temporary = index_path( );
		temporary += std::string{ snapshot_detail::TEMPORARY_SUFFIX };

		{
			auto out = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !out ) {
				return std::unexpected( fail( errc::io, "cannot rewrite the snapshot index" ) );
			}

			for ( const auto& entry : entries ) {
				auto line = snapshot_detail::entry_line( entry );

				out.write( line.data( ), static_cast< std::streamsize >( line.size( ) ) );
			}

			out.flush( );

			if ( !out ) {
				return std::unexpected(
					fail( errc::io, "rewriting the snapshot index did not complete" ) );
			}
		}

		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( index_path( ) ), error_code );

		if ( error_code ) {
			auto ignored = std::error_code{ };
			std::filesystem::remove( platform::to_extended_path( temporary ), ignored );

			return std::unexpected( fail( errc::io,
				"cannot replace the snapshot index: " + error_code.message( ) ) );
		}

		return status{ };
	}

	auto snapshot_store::evict_over_cap( const std::vector< index_entry >& entries ) -> status {
		auto run_order = std::vector< std::string >{ };
		auto seen = std::set< std::string, std::less<> >{ };

		for ( const auto& entry : entries ) {
			if ( seen.insert( entry.run ).second ) {
				run_order.push_back( entry.run );
			}
		}

		if ( run_order.size( ) <= MAX_RETAINED_RUNS ) {
			return status{ };
		}

		auto evicted = std::set< std::string, std::less<> >{ };
		const auto overflow = run_order.size( ) - MAX_RETAINED_RUNS;

		for ( auto index = std::size_t{ 0 }; index < overflow; ++index ) {
			evicted.insert( run_order[ index ] );
		}

		auto kept = std::vector< index_entry >{ };
		auto referenced = std::set< std::string, std::less<> >{ };

		for ( const auto& entry : entries ) {
			if ( evicted.contains( entry.run ) ) {
				continue;
			}

			if ( !entry.blob.empty( ) ) {
				referenced.insert( entry.blob );
			}

			kept.push_back( entry );
		}

		auto rewritten = rewrite_index( kept );

		if ( !rewritten ) {
			return rewritten;
		}

		for ( const auto& entry : entries ) {
			// A blob still named by a retained entry belongs to that entry, not to the evicted one.
			if ( !evicted.contains( entry.run ) || entry.blob.empty( ) ||
				referenced.contains( entry.blob ) ) {
				continue;
			}

			auto error_code = std::error_code{ };
			std::filesystem::remove( platform::to_extended_path( blob_path( entry.blob ) ),
				error_code );
		}

		return status{ };
	}

	auto snapshot_store::capture( const std::filesystem::path& workspace_root,
		const std::filesystem::path& file, const std::string_view run_id ) -> status {
		auto relative = workspace_relative_path( workspace_root, file );

		if ( !relative ) {
			return std::unexpected( relative.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( file ),
			error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot stat " + file.string( ) + ": " + error_code.message( ) ) );
		}

		auto existing = load_index( );

		if ( !existing ) {
			return std::unexpected( existing.error( ) );
		}

		auto entry = index_entry{ };
		entry.run = std::string{ run_id };
		entry.path = *relative;
		entry.existed = exists;
		entry.sequence = 0;

		for ( const auto& prior : *existing ) {
			entry.sequence = std::max( entry.sequence, prior.sequence + 1 );
		}

		if ( exists ) {
			const auto size = std::filesystem::file_size( platform::to_extended_path( file ),
				error_code );

			if ( error_code ) {
				return std::unexpected( fail( errc::io,
					"cannot size " + file.string( ) + ": " + error_code.message( ) ) );
			}

			if ( size > snapshot_detail::MAX_SNAPSHOT_FILE_BYTES ) {
				return std::unexpected(
					fail( errc::io, "file is too large to snapshot: " + file.string( ) ) );
			}

			auto bytes = snapshot_detail::read_whole_file( file );

			if ( !bytes ) {
				return std::unexpected( bytes.error( ) );
			}

			entry.blob = snapshot_detail::content_address( *bytes );

			auto stored = write_blob( entry.blob, *bytes );

			if ( !stored ) {
				return stored;
			}
		}

		auto appended = append_entry( entry );

		if ( !appended ) {
			return appended;
		}

		existing->push_back( entry );

		return evict_over_cap( *existing );
	}

}
