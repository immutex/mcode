#include "mcode/support/config.hxx"

#include <algorithm>

#include <cstdlib>
#include <fstream>
#include <sstream>

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

		// An unknown section is a typo. `timout = 5` at the root, or `[agnet]`,
		// would otherwise be stored and never read, so the user would believe the
		// setting applied.
		//
		// Not applied to project scope: the rule below refuses anything outside two
		// prefixes, so it already subsumes this one -- and its message is the one
		// that tells a user what a cloned repo is allowed to do.
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
		}

		// Project scope may only add restrictions. Anything else is rejected here,
		// at load, rather than being dropped silently later -- a project file that
		// appears to widen a permission but does not is the confusing case, and a
		// project file that DOES widen one is the dangerous case.
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

				// The name alone is not enough. `permissions.deny = "everything"` is
				// valid TOML and names a permitted key, and it collides with the
				// user's `permissions.deny` list in merge -- every consumer reads the
				// result through get_string_array, which turns the type mismatch into
				// an empty list, so the user's deny rules disappear and the blocked
				// tool becomes callable. Only a list may occupy a whole prefix;
				// anything BELOW a prefix is an addition and is always allowed.
				const auto exact_prefix = std::any_of( PROJECT_WRITABLE_PREFIXES.begin( ),
					PROJECT_WRITABLE_PREFIXES.end( ),
					[&]( const std::string_view prefix ) { return key == prefix; } );

				if ( exact_prefix && entry.kind != toml::value_kind::array ) {
					return std::unexpected( fail( errc::config,
						path.string( ) + ": project scope may only add to '" + key +
						"'; a list is required, not a single value" ) );
				}
			}
		}

		return layer{ .level = level, .origin = path, .values = std::move( *parsed ) };
	}

	auto merged_config::merge( std::vector< layer > layers ) -> result< merged_config > {
		auto out = merged_config{ };

		// Callers pass layers in whatever order they discovered them; sort by
		// precedence so the merge is order-independent.
		std::stable_sort( layers.begin( ), layers.end( ),
			[]( const layer& left, const layer& right ) {
				return static_cast< int >( left.level ) < static_cast< int >( right.level );
			} );

		for ( auto& source : layers ) {
			out.loaded_.push_back( source.level );

			for ( auto& [ key, entry ] : source.values.keys( ) ) {
				const auto existing = out.values_.find( key );

				if ( existing != out.values_.end( ) && existing->second.kind == toml::value_kind::array ) {
					// Lists merge; later entries append. Overriding an array
					// wholesale would make a project file's extra deny rule erase the
					// user's, which is the opposite of "may only add".
					if ( entry.kind != toml::value_kind::array ) {
						// A non-list cannot merge with a list, and overwriting would
						// silently drop restrictions. Fail closed rather than pick a
						// winner.
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

	auto merged_config::get_string( const std::string_view key ) const -> std::optional< std::string > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto text = found->second.as_string( ) ) {
			return *text;
		}

		return std::nullopt;
	}

	auto merged_config::get_int( const std::string_view key ) const -> std::optional< std::int64_t > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return std::nullopt;
		}

		if ( auto number = found->second.as_int( ) ) {
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
		-> std::vector< std::string > {
		const auto found = values_.find( std::string{ key } );

		if ( found == values_.end( ) ) {
			return { };
		}

		if ( auto items = found->second.as_string_array( ) ) {
			return *items;
		}

		return { };
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

		// Managed first, so a user or project layer can override it.
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
