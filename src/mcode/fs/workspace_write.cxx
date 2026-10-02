#include "mcode/fs/workspace.hxx"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/platform/seams.hxx"
#include "mcode/support/time.hxx"

namespace mcode {

	auto workspace::is_protected( const std::filesystem::path& absolute ) const -> bool {
		auto error_code = std::error_code{ };
		const auto canonical = std::filesystem::weakly_canonical( absolute, error_code );

		if ( error_code ) {
			return false;
		}

		auto root_entry = canonical_root_.begin( );
		auto path_entry = canonical.begin( );

		for ( ; root_entry != canonical_root_.end( ); ++root_entry, ++path_entry ) {
			if ( path_entry == canonical.end( ) ) {
				return false;
			}

			if ( *root_entry != *path_entry ) {
				return false;
			}
		}

		// The first component below the root: `notes.mcode` is not protected, `.mcode/` is.
		if ( path_entry == canonical.end( ) ) {
			return false;
		}

		const auto first = path_entry->string( );

		return first == ".mcode" || first == ".git";
	}

	auto workspace::write_file( const std::string_view relative_path, const std::string_view content,
		const write_mode mode ) -> result< write_receipt > {
		if ( content.size( ) > MAX_WRITE_FILE_BYTES ) {
			return std::unexpected( fail( errc::io, "content exceeds the " +
				std::to_string( MAX_WRITE_FILE_BYTES ) + "-byte write cap" ) );
		}

		auto resolved = resolve( relative_path );

		if ( !resolved ) {
			return std::unexpected( resolved.error( ) );
		}

		auto error_code = std::error_code{ };
		const auto exists = std::filesystem::exists( platform::to_extended_path( *resolved ), error_code );

		if ( error_code ) {
			return std::unexpected( fail( errc::io,
				"cannot stat " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		if ( mode == write_mode::create && exists ) {
			return std::unexpected( fail( errc::io,
				"file already exists: " + resolved->string( ) ) );
		}

		if ( mode == write_mode::overwrite && !exists ) {
			return std::unexpected( fail( errc::io,
				"file does not exist: " + resolved->string( ) ) );
		}

		if ( mode == write_mode::create ) {
			const auto parent = resolved->parent_path( );

			if ( !parent.empty( ) ) {
				std::filesystem::create_directories( platform::to_extended_path( parent ), error_code );

				if ( error_code ) {
					return std::unexpected( fail( errc::io, "cannot create " + parent.string( ) +
						": " + error_code.message( ) ) );
				}
			}
		}

		// A sibling temp file keeps the rename same-filesystem, hence atomic.
		static auto counter = std::atomic< std::uint64_t >{ 0 };

		const auto stamp = std::to_string( support::epoch_milliseconds( ) ) + "-" +
			std::to_string( counter.fetch_add( 1, std::memory_order_relaxed ) );

		auto temporary = *resolved;
		temporary += ".mcode-tmp-" + stamp;

		{
			auto output = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !output ) {
				return std::unexpected( fail( errc::io, "cannot open " + temporary.string( ) ) );
			}

			output.write( content.data( ), static_cast< std::streamsize >( content.size( ) ) );

			if ( !output ) {
				output.close( );
				std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

				return std::unexpected( fail( errc::io,
					"failed to write " + temporary.string( ) ) );
			}

			output.close( );

			if ( !output ) {
				std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

				return std::unexpected( fail( errc::io,
					"failed to flush " + temporary.string( ) ) );
			}
		}

		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( *resolved ), error_code );

		if ( error_code ) {
			std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

			return std::unexpected( fail( errc::io,
				"cannot replace " + resolved->string( ) + ": " + error_code.message( ) ) );
		}

		auto receipt = write_receipt{ };
		receipt.bytes_written = content.size( );
		receipt.content_hash = hash_bytes( content );

		return receipt;
	}

}
