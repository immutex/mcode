#include "mcode/ext/manifest.hxx"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "mcode/support/toml.hxx"

namespace mcode::ext {

	namespace {

		const auto KNOWN_PERMISSIONS = std::vector< std::string_view >{
			"fs_read", "fs_write", "net", "spawn", "mcp", "context", "session_fork",
		};

		const auto ALLOWED_KEYS = std::vector< std::string_view >{
			"name", "version", "api_version", "description", "permissions",
		};

		auto read_text( const std::filesystem::path& path ) -> result< std::string > {
			auto stream = std::ifstream{ path, std::ios::binary };

			if ( !stream ) {
				return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
			}

			auto buffer = std::ostringstream{ };
			buffer << stream.rdbuf( );

			return buffer.str( );
		}

		auto is_valid_name( const std::string_view name ) -> bool {
			// ^[a-z0-9]+(-[a-z0-9]+)*$, <=64 chars.
			if ( name.empty( ) || name.size( ) > 64 ) {
				return false;
			}

			auto previous_was_dash = true;

			for ( const auto character : name ) {
				if ( character == '-' ) {
					if ( previous_was_dash ) {
						return false;
					}

					previous_was_dash = true;

					continue;
				}

				if ( ( character < 'a' || character > 'z' ) &&
					( character < '0' || character > '9' ) ) {
					return false;
				}

				previous_was_dash = false;
			}

			return !previous_was_dash;
		}

		inline constexpr auto MAX_VERSION_DIGITS = 9;

		inline constexpr auto MAX_DESCRIPTION_LENGTH = std::size_t{ 1024 };

		auto is_valid_version( const std::string_view version ) -> bool {
			// MAJOR.MINOR.PATCH with an optional prerelease or build suffix; not full semver.
			auto parts = std::vector< std::int64_t >{ };
			auto current = std::int64_t{ 0 };
			auto digits = 0;
			auto suffix = std::string_view{ };
			auto index = std::size_t{ 0 };

			for ( ; index < version.size( ); ++index ) {
				const auto character = version[ index ];

				if ( character == '.' ) {
					if ( digits == 0 ) {
						return false;
					}

					parts.push_back( current );
					current = 0;
					digits = 0;

					continue;
				}

				if ( character == '-' || character == '+' ) {
					suffix = version.substr( index + 1 );

					break;
				}

				if ( character < '0' || character > '9' ) {
					return false;
				}

				// bounded before the multiply: `current * 10` overflows for a long component.
				if ( digits >= MAX_VERSION_DIGITS ) {
					return false;
				}

				current = current * 10 + ( character - '0' );
				++digits;
			}

			if ( digits == 0 ) {
				return false;
			}

			parts.push_back( current );

			if ( parts.size( ) != 3 ) {
				return false;
			}

			if ( suffix.empty( ) ) {
				return index < version.size( ) ? false : true;
			}

			auto identifier_length = std::size_t{ 0 };

			for ( const auto character : suffix ) {
				const auto alphanumeric = std::isalnum( static_cast< unsigned char >( character ) ) != 0;

				if ( character == '.' ) {
					if ( identifier_length == 0 ) {
						return false;
					}

					identifier_length = 0;

					continue;
				}

				if ( !alphanumeric && character != '-' ) {
					return false;
				}

				++identifier_length;
			}

			return identifier_length > 0;
		}

	}

	auto known_permissions( ) -> const std::vector< std::string_view >& {
		return KNOWN_PERMISSIONS;
	}

	auto manifest::has_permission( const std::string_view permission ) const noexcept -> bool {
		return std::find( permissions.begin( ), permissions.end( ), permission ) != permissions.end( );
	}

	auto load_manifest( const std::filesystem::path& directory ) -> result< manifest > {
		const auto path = directory / "ext.toml";

		auto text = read_text( path );

		if ( !text ) {
			return std::unexpected( fail( errc::io, "no manifest at " + path.string( ) ) );
		}

		auto parsed = toml::parse( *text );

		if ( !parsed ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": " + parsed.error( ).msg ) );
		}

		if ( auto known = parsed->reject_unknown( ALLOWED_KEYS ); !known ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": " + known.error( ).msg ) );
		}

		auto manifest_value = manifest{ };

		auto name = parsed->get_string( "name" );

		if ( !name ) {
			return std::unexpected( fail( errc::config, path.string( ) + ": 'name' is required" ) );
		}

		manifest_value.name = *name;

		if ( !is_valid_name( manifest_value.name ) ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": invalid name '" + manifest_value.name +
				"' (expected lowercase-kebab-case, <=64 chars)" ) );
		}

		// the name must match the directory, so identity cannot be claimed from two folders.
		if ( directory.filename( ).string( ) != manifest_value.name ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": name '" + manifest_value.name +
				"' does not match the directory '" + directory.filename( ).string( ) + "'" ) );
		}

		auto version = parsed->get_string( "version" );

		if ( !version ) {
			return std::unexpected( fail( errc::config, path.string( ) + ": 'version' is required" ) );
		}

		manifest_value.version = *version;

		if ( !is_valid_version( manifest_value.version ) ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": invalid version '" + manifest_value.version +
				"' (expected MAJOR.MINOR.PATCH)" ) );
		}

		auto api_version = parsed->get_int( "api_version" );

		if ( !api_version ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": 'api_version' is required and must be an integer" ) );
		}

		manifest_value.api_version = *api_version;

		if ( manifest_value.api_version < 1 ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": api_version must be >= 1" ) );
		}

		// an integer floor, never a range: the loader compares two integers.
		if ( manifest_value.api_version > API_VERSION ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": requires api_version " + std::to_string( manifest_value.api_version ) +
				", this build provides " + std::to_string( API_VERSION ) ) );
		}

		auto description = parsed->optional_string( "description" );

		if ( !description ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": " + description.error( ).msg ) );
		}

		if ( *description ) {
			if ( ( *description )->size( ) > MAX_DESCRIPTION_LENGTH ) {
				return std::unexpected( fail( errc::config,
					path.string( ) + ": description exceeds " +
					std::to_string( MAX_DESCRIPTION_LENGTH ) + " characters" ) );
			}

			manifest_value.description = **description;
		}

		// absent or empty means deny.
		if ( parsed->contains( "permissions" ) ) {
			auto permissions = parsed->get_string_array( "permissions" );

			if ( !permissions ) {
				return std::unexpected( fail( errc::config,
					path.string( ) + ": 'permissions' must be an array of strings" ) );
			}

			for ( const auto& permission : *permissions ) {
				const auto known = std::find( KNOWN_PERMISSIONS.begin( ), KNOWN_PERMISSIONS.end( ),
					permission );

				if ( known == KNOWN_PERMISSIONS.end( ) ) {
					return std::unexpected( fail( errc::config,
						path.string( ) + ": unknown permission '" + permission + "'" ) );
				}

				manifest_value.permissions.push_back( permission );
			}
		}

		return manifest_value;
	}

}
