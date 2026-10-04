#include "mcode/support/config.hxx"

#include <algorithm>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace mcode::config {

	namespace {

		auto read_file( const std::filesystem::path& path ) -> result< std::string > {
			auto stream = std::ifstream{ path, std::ios::binary };

			if ( !stream ) {
				return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
			}

			auto buffer = std::ostringstream{ };
			buffer << stream.rdbuf( );

			return buffer.str( );
		}

		auto environment_path( const char* name ) -> std::optional< std::filesystem::path > {
			const auto* value = std::getenv( name );

			if ( value == nullptr || *value == '\0' ) {
				return std::nullopt;
			}

			return std::filesystem::path{ value };
		}

		// only these two keys are read; a deeper permissions key would be silently dropped
		auto validate_permission_key( const std::string_view key, const toml::value& entry )
			-> status {
			const auto under_permissions = key == "permissions" ||
				key.starts_with( "permissions." );

			if ( !under_permissions ) {
				return { };
			}

			if ( key != "permissions.deny" && key != "permissions.ask" ) {
				return std::unexpected( fail( errc::config, "unknown key '" + std::string{ key } +
					"'; permission rules live at 'permissions.deny' and 'permissions.ask'" ) );
			}

			if ( entry.kind != toml::value_kind::array ) {
				return std::unexpected( fail( errc::config, "'" + std::string{ key } +
					"' must be a list of strings" ) );
			}

			return { };
		}

	}


	auto load_layer( const scope level, const std::filesystem::path& path ) -> result< layer > {
		auto text = read_file( path );

		if ( !text ) {
			return std::unexpected( text.error( ) );
		}

		auto parsed = toml::parse( *text );

		if ( !parsed ) {
			return std::unexpected( fail( errc::config,
				path.string( ) + ": " + parsed.error( ).msg ) );
		}

		// an unknown section is a typo, and a stored typo looks like it applied.
		for ( const auto& [ key, entry ] : parsed->keys( ) ) {
			if ( level == scope::project ) {
				break;
			}

			const auto section = key.substr( 0, key.find( '.' ) );

			auto known = false;

			for ( const auto prefix : CONFIG_SECTIONS ) {
				if ( section == prefix ) {
					known = true;

					break;
				}
			}

			if ( !known ) {
				return std::unexpected( fail( errc::config, path.string( ) + ": unknown key '" +
					key + "'; no config section is named '" + std::string{ section } + "'" ) );
			}

			if ( const auto checked = validate_permission_key( key, entry ); !checked ) {
				return std::unexpected( fail( errc::config,
					path.string( ) + ": " + checked.error( ).msg ) );
			}
		}

		// project scope may only add restrictions; anything else is rejected at load.
		if ( level == scope::project ) {
			for ( const auto& [ key, entry ] : parsed->keys( ) ) {
				auto permitted = false;

				for ( const auto prefix : PROJECT_WRITABLE_PREFIXES ) {
					if ( key == prefix || key.starts_with( std::string{ prefix } + "." ) ) {
						permitted = true;

						break;
					}
				}

				if ( !permitted ) {
					return std::unexpected( fail( errc::config,
						path.string( ) + ": project scope may not set '" + key +
						"'; it may only add 'permissions.deny' or 'permissions.ask'" ) );
				}

				if ( const auto checked = validate_permission_key( key, entry ); !checked ) {
					return std::unexpected( fail( errc::config,
						path.string( ) + ": " + checked.error( ).msg ) );
				}
			}
		}

		return layer{ .level = level, .origin = path, .values = std::move( *parsed ) };
	}

	auto merged_config::merge( std::vector< layer > layers ) -> result< merged_config > {
		auto out = merged_config{ };

		std::stable_sort( layers.begin( ), layers.end( ),
			[]( const layer& left, const layer& right ) {
				return static_cast< int >( left.level ) < static_cast< int >( right.level );
			} );

		for ( auto& source : layers ) {
			out.loaded_.push_back( source.level );

			for ( auto& [ key, entry ] : source.values.keys( ) ) {
				const auto existing = out.values_.find( key );

				if ( existing != out.values_.end( ) &&
					existing->second.kind == toml::value_kind::array ) {
					// A wholesale override would erase the user's entries.
					if ( entry.kind != toml::value_kind::array ) {
						return std::unexpected( fail( errc::config,
							"layer " + std::to_string( static_cast< int >( source.level ) ) +
							" sets '" + key + "' to a single value, but it is a list" ) );
					}

					for ( const auto& item : entry.items ) {
						existing->second.items.push_back( item );
					}

					out.origins_[ key ] = source.level;

					continue;
				}

				out.values_[ key ] = entry;
				out.origins_[ key ] = source.level;
			}
		}

		return out;
	}

	auto merged_config::get_string( const std::string_view key ) const
		-> std::optional< std::string > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto text = found->second.as_string( ) ) {
			return *text;
		}

		return std::nullopt;
	}

	auto merged_config::get_int( const std::string_view key ) const
		-> std::optional< std::int64_t > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto number = found->second.as_int( ) ) {
			return *number;
		}

		return std::nullopt;
	}

	auto merged_config::get_double( const std::string_view key ) const -> std::optional< double > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto number = found->second.as_double( ) ) {
			return *number;
		}

		return std::nullopt;
	}

	auto merged_config::get_bool( const std::string_view key ) const -> std::optional< bool > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto flag = found->second.as_bool( ) ) {
			return *flag;
		}

		return std::nullopt;
	}

	auto merged_config::get_string_array( const std::string_view key ) const
		-> result< std::vector< std::string > > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::vector< std::string >{ };
		}

		if ( auto items = found->second.as_string_array( ) ) {
			return std::move( *items );
		}

		return std::unexpected( fail( errc::config,
			"'" + std::string{ key } + "' must be a list of strings" ) );
	}

	auto merged_config::source_of( const std::string_view key ) const -> std::optional< scope > {
		const auto found = origins_.find( std::string{ key } );

		if ( found == origins_.end( ) ) {
			return std::nullopt;
		}

		return found->second;
	}

	auto default_layer_paths( )
		-> std::vector< std::pair< scope, std::filesystem::path > > {
		auto out = std::vector< std::pair< scope, std::filesystem::path > >{ };

	#if defined( _WIN32 )
		if ( auto program_data = environment_path( "PROGRAMDATA" ) ) {
			out.emplace_back( scope::managed, *program_data / "mcode" / "config.toml" );
		}

		if ( auto app_data = environment_path( "APPDATA" ) ) {
			out.emplace_back( scope::user, *app_data / "mcode" / "config.toml" );
		}
	#else
		if ( auto xdg_config = environment_path( "XDG_CONFIG_HOME" ) ) {
			out.emplace_back( scope::user, *xdg_config / "mcode" / "config.toml" );
		} else if ( auto home = environment_path( "HOME" ) ) {
			out.emplace_back( scope::user, *home / ".config" / "mcode" / "config.toml" );
		}
	#endif

		return out;
	}

	auto load( const std::filesystem::path& project_root ) -> result< merged_config > {
		auto layers = std::vector< layer >{ };

		for ( const auto& [ level, path ] : default_layer_paths( ) ) {
			if ( !std::filesystem::exists( path ) ) {
				continue;
			}

			auto loaded = load_layer( level, path );

			if ( !loaded ) {
				return std::unexpected( loaded.error( ) );
			}

			layers.push_back( std::move( *loaded ) );
		}

		if ( !project_root.empty( ) ) {
			const auto path = project_root / ".mcode" / "config.toml";

			if ( std::filesystem::exists( path ) ) {
				auto loaded = load_layer( scope::project, path );

				if ( !loaded ) {
					return std::unexpected( loaded.error( ) );
				}

				layers.push_back( std::move( *loaded ) );
			}
		}

		return merged_config::merge( std::move( layers ) );
	}

}
