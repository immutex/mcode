#include "mcode/ext/loader.hxx"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "mcode/support/toml.hxx"

namespace mcode::ext {

	auto load_report::total_tools( ) const noexcept -> std::size_t {
		auto count = std::size_t{ 0 };

		for ( const auto& entry : loaded ) {
			count += entry.tools.size( );
		}

		return count;
	}

	auto loaded_extension::tool_names( ) const -> std::vector< std::string > {
		auto names = std::vector< std::string >{ };

		if ( surface != nullptr ) {
			for ( const auto& tool : surface->tools( ) ) {
				names.push_back( tool.name );
			}
		}

		return names;
	}

	auto load_result::find( const std::string_view name ) const -> const loaded_extension* {
		for ( const auto& extension : extensions ) {
			if ( extension.details.name == name ) {
				return &extension;
			}
		}

		return nullptr;
	}

	auto load_result::invoke( const std::string_view tool_name,
		const std::string_view arguments_json ) -> result< std::string > {
		for ( const auto& extension : extensions ) {
			if ( extension.surface == nullptr ) {
				continue;
			}

			for ( const auto& tool : extension.surface->tools( ) ) {
				if ( tool.name == tool_name ) {
					return extension.surface->invoke( tool_name, arguments_json );
				}
			}
		}

		return std::unexpected( fail( errc::config,
			"no loaded extension provides a tool named '" + std::string{ tool_name } + "'" ) );
	}

	auto load_extensions( const std::vector< std::filesystem::path >& roots,
		model::provider_registry& providers, hook_registry& hooks,
		const loader_options& options ) -> load_result {
		auto outcome = load_result{ };

		// Collect candidates first, so a disabled extension is counted rather than
		// silently absent. "Disabled" and "not installed" are different answers to
		// the user's question.
		struct candidate {
			std::filesystem::path directory;
			std::string name;
		};

		auto candidates = std::vector< candidate >{ };

		for ( const auto& root : roots ) {
			if ( !std::filesystem::exists( root ) ) {
				continue;
			}

			auto error = std::error_code{ };

			for ( const auto& entry : std::filesystem::directory_iterator{ root, error } ) {
				if ( !entry.is_directory( ) ) {
					continue;
				}

				if ( !std::filesystem::exists( entry.path( ) / "ext.toml" ) ) {
					continue;
				}

				candidates.push_back( { entry.path( ), entry.path( ).filename( ).string( ) } );
			}
		}

		// Deterministic order: two runs must load in the same sequence, or a
		// name-collision report would differ run to run.
		std::sort( candidates.begin( ), candidates.end( ),
			[]( const candidate& left, const candidate& right ) {
				return left.name < right.name;
			} );

		for ( const auto& candidate : candidates ) {
			if ( options.disabled ) {
				++outcome.report.disabled;

				continue;
			}

			if ( std::find( options.disabled_names.begin( ), options.disabled_names.end( ),
				candidate.name ) != options.disabled_names.end( ) ) {
				++outcome.report.disabled;

				continue;
			}

			auto manifest_value = load_manifest( candidate.directory );

			if ( !manifest_value ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					manifest_value.error( ).msg } );

				continue;
			}

			// A second extension with the same name would make attribution
			// ambiguous, so it is refused rather than allowed to shadow.
			if ( outcome.find( manifest_value->name ) != nullptr ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"duplicate extension name; another directory already claimed it" } );

				continue;
			}

			auto host_options = lua_host_options{ };
			host_options.extension_name = manifest_value->name;
			host_options.memory_limit_bytes = options.memory_limit_bytes;
			host_options.time_limit = options.time_limit;

			auto created = lua_host::create( std::move( host_options ) );

			if ( !created ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"could not create the VM: " + created.error( ).msg } );

				continue;
			}

			// Owned from here, and never moved again: the API surface stores a
			// pointer to the VM, so moving the host afterwards would leave it
			// dangling and every tool call would fail with "no thread".
			auto host = std::make_unique< lua_host >( std::move( *created ) );

			auto surface = std::make_unique< api_surface >( );

			if ( options.register_api ) {
				// The surface is installed before the extension runs, because
				// sealing is a one-way door: registering after the first execution
				// returns an error rather than silently failing.
				if ( auto registered = options.register_api( *host, *surface, providers, hooks,
					*manifest_value ); !registered ) {
					outcome.report.failed.push_back( { candidate.name, candidate.directory,
						"API registration failed: " + registered.error( ).msg } );

					continue;
				}
			}

			const auto entry_point = candidate.directory / "init.luau";

			if ( !std::filesystem::exists( entry_point ) ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"no init.luau" } );

				continue;
			}

			// Registration must be cheap. A failure here is a bug in the extension,
			// reported rather than hung on.
			if ( auto ran = host->run_file( entry_point ); !ran ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					ran.error( ).msg } );

				continue;
			}

			auto entry = loaded_extension{ };
			entry.details = *manifest_value;
			entry.host = std::move( host );
			entry.surface = std::move( surface );

			auto report_entry = load_outcome{ };
			report_entry.name = entry.details.name;
			report_entry.directory = candidate.directory;
			report_entry.bytes_used = entry.host->bytes_allocated( );
			report_entry.tools = entry.tool_names( );
			report_entry.warnings = entry.surface->refusals( );

			outcome.report.loaded.push_back( std::move( report_entry ) );
			outcome.extensions.push_back( std::move( entry ) );
		}

		return outcome;
	}

	auto default_register_api( tool_registry& registry, model::provider_registry& providers )
		-> std::function< status( lua_host& host, api_surface& surface,
			model::provider_registry& providers, hook_registry& hooks,
			const manifest& manifest ) > {
		// Every registry is captured by reference, not copied: the surface writes
		// into them directly, and a copy would leave the caller's registry empty
		// while the extension appeared to load.
		return [ &registry ]( lua_host& host, api_surface& surface,
			model::provider_registry& declared, hook_registry& declared_hooks,
			const manifest& details ) -> status {
			return surface.install( host, registry, declared, declared_hooks, details );
		};
	}

	auto default_roots( const std::filesystem::path& workspace )
		-> std::vector< std::filesystem::path > {
		auto roots = std::vector< std::filesystem::path >{ };

		roots.push_back( workspace / ".mcode" / "extensions" );

		// The user root is resolved from the environment rather than a hardcoded
		// home path, because the platform seam owns that decision (docs/24).
		if ( const auto* home = std::getenv( "USERPROFILE" ); home != nullptr ) {
			roots.push_back( std::filesystem::path{ home } / ".mcode" / "extensions" );
		} else if ( const auto* home = std::getenv( "HOME" ); home != nullptr ) {
			roots.push_back( std::filesystem::path{ home } / ".mcode" / "extensions" );
		}

		return roots;
	}

	auto unload_extension( tool_registry& registry, const std::string_view owner ) -> std::size_t {
		return registry.remove_owner( owner );
	}

}
