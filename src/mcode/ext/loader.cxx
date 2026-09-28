#include "mcode/ext/loader.hxx"

#include <algorithm>
#include <array>
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
		const auto found = tool_owners.find( tool_name );

		if ( found == tool_owners.end( ) || found->second == nullptr ) {
			return std::unexpected( fail( errc::config,
				"no loaded extension provides a tool named '" + std::string{ tool_name } + "'" ) );
		}

		return found->second->invoke( tool_name, arguments_json );
	}

	namespace {

		// Undoes everything a failed extension registered, BEFORE its VM is
		// destroyed.
		//
		// An extension can register a tool, a provider, and a hook and then fail --
		// a typo further down init.luau is enough. The VM that owns those closures
		// is discarded with the extension, so leaving them registered means a tool
		// the model can see and cannot call, and a hook the bus dispatches into
		// freed memory. The owner name is the extension's manifest name, which is
		// why every registration path stamps it.
		auto discard( lua_host& host, api_surface& surface,
			model::provider_registry& providers, hook_registry& hooks ) -> void {
			const auto owner = host.name( );

			if ( owner.empty( ) ) {
				return;
			}

			// Hooks first: the host is still alive, so the closure references can be
			// released. After this the bus holds nothing pointing at the VM.
			hooks.detach_owner( host, owner );
			providers.remove_owner( owner );

			// The surface unregisters its own tools, because it is what registered
			// them and it holds the registry they went into.
			surface.release_all( host );
		}

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

			const auto entries = std::filesystem::directory_iterator{ root, error };

			// An unreadable root is not the same as an empty one. Reporting it
			// through the same channel as a failed extension is what stops a
			// permission problem from silently presenting itself as "no extensions
			// installed" -- the caller already prints this list.
			if ( error ) {
				outcome.report.failed.push_back( { root.string( ), root,
					"cannot read the extension root: " + error.message( ) } );

				continue;
			}

			for ( const auto& entry : entries ) {
				// symlink_status, not is_directory: is_directory follows a symlink,
				// so a link out of the root would load with the root's identity.
				auto status = std::error_code{ };
				const auto kind = entry.symlink_status( status );

				if ( status || !std::filesystem::is_directory( kind ) ) {
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

			// `require` resolves inside the extension directory and nowhere else.
			// A path that escapes the root is refused here, so the VM never reaches
			// the filesystem at large.
			const auto extension_root = std::filesystem::weakly_canonical( candidate.directory );
			host_options.module_loader = [ extension_root ]( const std::string_view module_path )
				-> std::optional< std::string > {
				auto relative = std::filesystem::path{ std::string{ module_path } };

				if ( relative.is_absolute( ) ) {
					return std::nullopt;
				}

				// The path form is documented as relative to the extension root,
				// with `init.luau` resolving a directory. Luau's `require("./lib/x")`
				// is the form authors write, so the leading `./` is dropped and the
				// bare, `.luau` and `init.luau` spellings are all tried.
				auto stripped = relative.lexically_normal( ).string( );

				while ( stripped.starts_with( "./" ) ) {
					stripped.erase( 0, 2 );
				}

				const auto candidates = std::array{
					std::filesystem::path{ stripped },
					std::filesystem::path{ stripped + ".luau" },
					std::filesystem::path{ stripped } / "init.luau",
				};

				for ( const auto& attempt : candidates ) {
					auto resolved = std::filesystem::weakly_canonical( extension_root / attempt );

					// The resolved path must still be under the extension root.
					// Compared component-wise, because a string prefix test accepts
					// `/ext-other` for root `/ext`.
					auto root_part = extension_root.begin( );
					auto resolved_part = resolved.begin( );
					auto contained = true;

					for ( ; root_part != extension_root.end( ); ++root_part, ++resolved_part ) {
						if ( resolved_part == resolved.end( ) || *root_part != *resolved_part ) {
							contained = false;

							break;
						}
					}

					if ( !contained ) {
						continue;
					}

					auto source = std::ifstream{ resolved, std::ios::binary };

					if ( !source ) {
						continue;
					}

					auto buffer = std::ostringstream{ };
					buffer << source.rdbuf( );

					return buffer.str( );
				}

				return std::nullopt;
			};

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
				if ( auto registered = options.register_api(
						registration{ .host = *host, .surface = *surface,
							.providers = providers, .hooks = hooks,
							.details = *manifest_value } ); !registered ) {
					outcome.report.failed.push_back( { candidate.name, candidate.directory,
						"API registration failed: " + registered.error( ).msg } );

					continue;
				}
			}

			const auto entry_point = candidate.directory / "init.luau";

			if ( !std::filesystem::exists( entry_point ) ) {
				discard( *host, *surface, providers, hooks );

				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"no init.luau" } );

				continue;
			}

			// Registration must be cheap. A failure here is a bug in the extension,
			// reported rather than hung on.
			if ( auto ran = host->run_file( entry_point ); !ran ) {
				discard( *host, *surface, providers, hooks );

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

			// Recorded before the move, so dispatch has an owner for every tool
			// without rescanning the extension list.
			for ( const auto& tool : entry.tool_names( ) ) {
				outcome.tool_owners.insert_or_assign( tool, entry.surface.get( ) );
			}

			outcome.report.loaded.push_back( std::move( report_entry ) );
			outcome.extensions.push_back( std::move( entry ) );
		}

		return outcome;
	}

	auto default_register_api( tool_registry& registry )
		-> std::function< status( const registration& ) > {
		// The tool registry is captured by reference, not copied: the surface writes
		// into it directly, and a copy would leave the caller's registry empty while
		// the extension appeared to load. Everything else arrives per call, because
		// the caller owns the ones it must inspect afterwards.
		return [ &registry ]( const registration& given ) -> status {
			return given.surface.install( api_surface::install_request{ .host = given.host,
				.registry = registry, .providers = given.providers, .hooks = given.hooks,
				.details = given.details } );
		};
	}

	auto default_roots( const std::filesystem::path& workspace )
		-> std::vector< std::filesystem::path > {
		auto roots = std::vector< std::filesystem::path >{ };

		roots.push_back( workspace / ".mcode" / "extensions" );

		// The user root is resolved from the environment rather than a hardcoded
		// home path, because the platform seam owns that decision.
		if ( const auto* profile = std::getenv( "USERPROFILE" ); profile != nullptr ) {
			roots.push_back( std::filesystem::path{ profile } / ".mcode" / "extensions" );
		} else if ( const auto* home = std::getenv( "HOME" ); home != nullptr ) {
			roots.push_back( std::filesystem::path{ home } / ".mcode" / "extensions" );
		}

		return roots;
	}

	auto unload_extension( tool_registry& registry, const std::string_view owner ) -> std::size_t {
		return registry.remove_owner( owner );
	}

}
