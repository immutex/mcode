#include "mcode/fs/snapshot.hxx"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mcode/fs/snapshot_internal.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode {

	namespace {

		// A relative index path may only ever name a descendant of the workspace root: a
		// tampered index must not point a restore at an absolute path or a parent directory.
		[[nodiscard]] auto join_inside( const std::filesystem::path& root,
			const std::string_view relative ) -> result< std::filesystem::path > {
			if ( relative.empty( ) ) {
				return std::unexpected( fail( errc::io, "empty snapshot path" ) );
			}

			auto candidate = std::filesystem::path{ std::string{ relative } };

			if ( candidate.is_absolute( ) ) {
				return std::unexpected( fail( errc::io,
					"snapshot path is absolute: " + candidate.string( ) ) );
			}

			for ( const auto& part : candidate ) {
				if ( part == ".." ) {
					return std::unexpected( fail( errc::io,
						"snapshot path escapes the workspace: " + candidate.string( ) ) );
				}
			}

			return root / candidate;
		}

	}

	auto snapshot_store::restore_entry( const std::filesystem::path& workspace_root,
		const index_entry& entry ) -> result< bool > {
		auto target = join_inside( workspace_root, entry.path );

		if ( !target ) {
			return std::unexpected( target.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( *target ),
			error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot stat " + target->string( ) + ": " + error_code.message( ) ) );
		}

		if ( !entry.existed ) {
			if ( !exists ) {
				return false;
			}

			std::filesystem::remove( platform::to_extended_path( *target ), error_code );

			if ( error_code ) {
				return std::unexpected( fail( errc::io,
					"cannot delete " + target->string( ) + ": " + error_code.message( ) ) );
			}

			return true;
		}

		auto bytes = std::string{ };

		if ( auto read = read_blob( entry.blob, bytes ); !read ) {
			return std::unexpected( read.error( ) );
		}

		if ( exists ) {
			auto current = snapshot_detail::read_whole_file( *target );

			if ( !current ) {
				return std::unexpected( current.error( ) );
			}

			// Bytes already equal to the capture are not a change, so the count stays honest.
			if ( *current == bytes ) {
				return false;
			}
		}

		std::filesystem::create_directories( platform::to_extended_path( target->parent_path( ) ),
			error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot create " + target->parent_path( ).string( ) + ": " +
				error_code.message( ) ) );
		}

		// A sibling temp file renamed over the target, the same rule the write tool follows,
		// so an interrupted restore cannot leave a half-written file behind.
		auto temporary = *target;
		temporary += std::string{ snapshot_detail::TEMPORARY_SUFFIX };

		{
			auto out = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !out ) {
				return std::unexpected( fail( errc::io, "cannot restore " + target->string( ) ) );
			}

			out.write( bytes.data( ), static_cast< std::streamsize >( bytes.size( ) ) );
			out.flush( );

			if ( !out ) {
				return std::unexpected( fail( errc::io,
					"restoring " + target->string( ) + " did not complete" ) );
			}
		}

		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( *target ), error_code );

		if ( error_code ) {
			auto ignored = std::error_code{ };
			std::filesystem::remove( platform::to_extended_path( temporary ), ignored );

			return std::unexpected( fail( errc::io,
				"cannot replace " + target->string( ) + ": " + error_code.message( ) ) );
		}

		return true;
	}

	auto snapshot_store::restore_selected( const std::filesystem::path& workspace_root,
		const std::vector< index_entry >& entries ) -> result< std::size_t > {
		// Newest capture per path wins, and the survivors are restored in reverse capture
		// order, so the most recent change to a file is the first one undone.
		auto newest = std::map< std::string, const index_entry*, std::less<> >{ };

		for ( const auto& entry : entries ) {
			const auto found = newest.find( entry.path );

			if ( found == newest.end( ) || entry.sequence > found->second->sequence ) {
				newest.insert_or_assign( entry.path, &entry );
			}
		}

		auto ordered = std::vector< const index_entry* >{ };

		for ( const auto& candidate : newest ) {
			ordered.push_back( candidate.second );
		}

		std::sort( ordered.begin( ), ordered.end( ),
			[]( const index_entry* left, const index_entry* right ) {
				return left->sequence > right->sequence;
			} );

		auto changed = std::size_t{ 0 };

		for ( const auto* entry : ordered ) {
			auto restored = restore_entry( workspace_root, *entry );

			if ( !restored ) {
				return std::unexpected( restored.error( ) );
			}

			if ( *restored ) {
				++changed;
			}
		}

		return changed;
	}

	auto snapshot_store::restore_last( const std::filesystem::path& workspace_root,
		const std::string_view run_id ) -> result< std::size_t > {
		auto entries = load_index( );

		if ( !entries ) {
			return std::unexpected( entries.error( ) );
		}

		auto selected = std::vector< index_entry >{ };

		for ( const auto& entry : *entries ) {
			if ( entry.run == run_id ) {
				selected.push_back( entry );
			}
		}

		return restore_selected( workspace_root, selected );
	}

	auto snapshot_store::restore_all( const std::filesystem::path& workspace_root )
		-> result< std::size_t > {
		auto entries = load_index( );

		if ( !entries ) {
			return std::unexpected( entries.error( ) );
		}

		return restore_selected( workspace_root, *entries );
	}

}
