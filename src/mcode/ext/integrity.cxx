#include "mcode/ext/integrity.hxx"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>

#include <openssl/evp.h>

#include "mcode/fs/workspace.hxx"

namespace mcode::ext {

	namespace {

		// Framed, so a path or a length cannot be read as part of the previous entry.
		constexpr std::string_view TREE_DOMAIN = "mcode.ext.tree.v1";

		auto append_u64( std::string& out, const std::uint64_t value ) -> void {
			for ( auto shift = 56; shift >= 0; shift -= 8 ) {
				out.push_back( static_cast< char >( ( value >> shift ) & 0xffu ) );
			}
		}

		auto read_whole_file( const std::filesystem::path& path, std::string& out ) -> bool {
			auto stream = std::ifstream{ path, std::ios::binary };

			if ( !stream ) {
				return false;
			}

			out.assign( std::istreambuf_iterator< char >{ stream },
				std::istreambuf_iterator< char >{ } );

			return stream.good( ) || stream.eof( );
		}

		// Never follows a link: a link is either an escape or an aliasing bug.
		auto collect_files( const std::filesystem::path& root,
			const std::filesystem::path& directory, std::vector< std::filesystem::path >& out )
			-> status {
			auto error = std::error_code{ };
			auto entries = std::filesystem::directory_iterator{ directory, error };

			if ( error ) {
				return std::unexpected( fail( errc::io,
					"cannot read " + directory.string( ) + ": " + error.message( ) ) );
			}

			for ( const auto& entry : entries ) {
				auto status_error = std::error_code{ };
				const auto kind = entry.symlink_status( status_error );

				if ( status_error ) {
					return std::unexpected( fail( errc::io,
						"cannot stat " + entry.path( ).string( ) ) );
				}

				if ( std::filesystem::is_symlink( kind ) ) {
					return std::unexpected( fail( errc::config,
						"extension contains a symlink, which is refused: "
							+ entry.path( ).string( ) ) );
				}

				if ( std::filesystem::is_directory( kind ) ) {
					if ( auto descended = collect_files( root, entry.path( ), out ); !descended ) {
						return descended;
					}

					continue;
				}

				if ( !std::filesystem::is_regular_file( kind ) ) {
					return std::unexpected( fail( errc::config,
						"extension contains a non-regular file: " + entry.path( ).string( ) ) );
				}

				out.push_back( entry.path( ).lexically_normal( ) );
			}

			return { };
		}

	}

	auto sha256_hex( const std::string_view bytes ) -> std::string {
		auto digest = std::array< unsigned char, EVP_MAX_MD_SIZE >{ };
		auto length = 0u;

		auto* context = ::EVP_MD_CTX_new( );

		if ( context == nullptr ) {
			return { };
		}

		const auto ok = ::EVP_DigestInit_ex( context, ::EVP_sha256( ), nullptr ) == 1
			&& ::EVP_DigestUpdate( context, bytes.data( ), bytes.size( ) ) == 1
			&& ::EVP_DigestFinal_ex( context, digest.data( ), &length ) == 1;

		::EVP_MD_CTX_free( context );

		if ( !ok ) {
			return { };
		}

		constexpr auto HEX = std::string_view{ "0123456789abcdef" };
		auto out = std::string{ };
		out.reserve( length * 2 );

		for ( auto index = 0u; index < length; ++index ) {
			out.push_back( HEX[ digest[ index ] >> 4 ] );
			out.push_back( HEX[ digest[ index ] & 0x0fu ] );
		}

		return out;
	}

	auto extension_snapshot::find( const std::string_view relative_path ) const
		-> const std::string* {
		for ( const auto& entry : files ) {
			if ( entry.relative_path == relative_path ) {
				return &entry.bytes;
			}
		}

		return nullptr;
	}

	auto snapshot_extension( const std::filesystem::path& root ) -> result< extension_snapshot > {
		auto error = std::error_code{ };
		const auto canonical = std::filesystem::weakly_canonical( root, error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot resolve " + root.string( ) + ": " + error.message( ) ) );
		}

		auto absolute = std::vector< std::filesystem::path >{ };

		if ( auto collected = collect_files( canonical, canonical, absolute ); !collected ) {
			return std::unexpected( collected.error( ) );
		}

		if ( absolute.size( ) > MAX_EXTENSION_FILES ) {
			return std::unexpected( fail( errc::config,
				"extension has " + std::to_string( absolute.size( ) ) + " files, over the limit of "
					+ std::to_string( MAX_EXTENSION_FILES ) ) );
		}

		auto snapshot = extension_snapshot{ };
		auto total = std::uintmax_t{ 0 };

		for ( const auto& full : absolute ) {
			if ( !path_is_within( canonical, full ) ) {
				return std::unexpected( fail( errc::config,
					"extension file escapes its root: " + full.string( ) ) );
			}

			auto size_error = std::error_code{ };
			const auto size = std::filesystem::file_size( full, size_error );

			if ( size_error ) {
				return std::unexpected( fail( errc::io,
					"cannot size " + full.string( ) + ": " + size_error.message( ) ) );
			}

			if ( size > MAX_EXTENSION_FILE_BYTES ) {
				return std::unexpected( fail( errc::config,
					full.filename( ).string( ) + " is over the per-file limit of "
						+ std::to_string( MAX_EXTENSION_FILE_BYTES ) + " bytes" ) );
			}

			total += size;

			if ( total > MAX_EXTENSION_TREE_BYTES ) {
				return std::unexpected( fail( errc::config,
					"extension tree is over the limit of "
						+ std::to_string( MAX_EXTENSION_TREE_BYTES ) + " bytes" ) );
			}

			auto entry = snapshot_entry{ };
			entry.relative_path = std::filesystem::relative( full, canonical, error ).generic_string( );

			if ( error ) {
				return std::unexpected( fail( errc::io,
					"cannot relativize " + full.string( ) + ": " + error.message( ) ) );
			}

			if ( !read_whole_file( full, entry.bytes ) ) {
				return std::unexpected( fail( errc::io, "cannot read " + full.string( ) ) );
			}

			entry.sha256 = sha256_hex( entry.bytes );

			if ( entry.sha256.empty( ) ) {
				return std::unexpected( fail( errc::io, "cannot hash " + full.string( ) ) );
			}

			snapshot.files.push_back( std::move( entry ) );
		}

		std::sort( snapshot.files.begin( ), snapshot.files.end( ),
			[]( const snapshot_entry& left, const snapshot_entry& right ) {
				return left.relative_path < right.relative_path;
			} );

		auto framed = std::string{ TREE_DOMAIN };
		append_u64( framed, static_cast< std::uint64_t >( snapshot.files.size( ) ) );

		for ( const auto& entry : snapshot.files ) {
			append_u64( framed, static_cast< std::uint64_t >( entry.relative_path.size( ) ) );
			framed.append( entry.relative_path );
			framed.append( entry.sha256 );
		}

		snapshot.tree_sha256 = sha256_hex( framed );

		return snapshot;
	}

	auto resolve_module( const extension_snapshot& snapshot, const std::string_view module_path )
		-> const std::string* {
		const auto relative = std::filesystem::path{ std::string{ module_path } };

		if ( relative.is_absolute( ) ) {
			return nullptr;
		}

		auto stripped = relative.lexically_normal( ).generic_string( );

		while ( stripped.starts_with( "./" ) ) {
			stripped.erase( 0, 2 );
		}

		if ( stripped.empty( ) || stripped.starts_with( "../" ) || stripped == ".." ) {
			return nullptr;
		}

		const auto candidates = std::array{
			stripped,
			stripped + ".luau",
			stripped + "/init.luau",
		};

		for ( const auto& candidate : candidates ) {
			if ( const auto* found = snapshot.find( candidate ); found != nullptr ) {
				return found;
			}
		}

		return nullptr;
	}

}
