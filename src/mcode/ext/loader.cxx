#include "mcode/ext/loader.hxx"

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "mcode/ext/integrity.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::ext {

	auto to_string( const load_trust value ) noexcept -> std::string_view {
		switch ( value ) {
			case load_trust::trusted_by_location: return "trusted-by-location";
			case load_trust::granted: return "granted";
			case load_trust::ungranted: return "ungranted";
			case load_trust::denied: return "denied";
			case load_trust::changed: return "changed";
		}

		return "ungranted";
	}

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

	// every loaded surface, on the caller's thread: timers never fire concurrently with a hook.
	auto load_result::pump_timers( ) -> void {
		for ( auto& extension : extensions ) {
			if ( extension.surface != nullptr ) {
				extension.surface->pump_timers( );
			}
		}
	}

	namespace {

		// runs before the VM is destroyed: the closures it registered live in that VM.
		auto discard( lua_host& host, api_surface& surface,
			model::provider_registry& providers, hook_registry& hooks ) -> void {
			const auto owner = host.name( );

			if ( owner.empty( ) ) {
				return;
			}

			hooks.detach_owner( host, owner );
			providers.remove_owner( owner );

			surface.release_all( host );
		}

	}

	auto load_extensions( const std::vector< std::filesystem::path >& roots,
		model::provider_registry& providers, hook_registry& hooks,
		const loader_options& options ) -> load_result {
		auto outcome = load_result{ };

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

			if ( error ) {
				outcome.report.failed.push_back( { root.string( ), root,
					"cannot read the extension root: " + error.message( ) } );

				continue;
			}

			for ( const auto& entry : entries ) {
				// symlink_status, not is_directory: the latter follows a link out of the root.
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

			if ( outcome.find( manifest_value->name ) != nullptr ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"duplicate extension name; another directory already claimed it" } );

				continue;
			}

			// A candidate under the workspace's own extension root arrived with the
			// repository, so it runs only against a grant. Everything else was put
			// there by the user or shipped beside the binary.
			const auto is_project = !options.project_root.empty( )
				&& path_is_within( options.project_root, candidate.directory );

			auto trust = load_trust::trusted_by_location;

			if ( is_project ) {
				const auto decision = options.trust != nullptr
					? options.trust->decide( trust_scope::project, manifest_value->name,
						manifest_value->api_version )
					: trust_decision::ungranted;

				if ( decision == trust_decision::denied ) {
					outcome.report.inert.push_back( { manifest_value->name, candidate.directory,
						load_trust::denied, "denied by the trust store" } );

					continue;
				}

				if ( decision == trust_decision::ungranted ) {
					outcome.report.inert.push_back( { manifest_value->name, candidate.directory,
						load_trust::ungranted,
						"a repository extension runs only against a grant; run 'mcode ext trust "
							+ manifest_value->name + "'" } );

					continue;
				}

				trust = load_trust::granted;
			}

			// Read every file once. The bytes hashed here are the bytes compiled
			// below, so there is no second read and no window between the two.
			auto snapshot = snapshot_extension( candidate.directory );

			if ( !snapshot ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					snapshot.error( ).msg } );

				continue;
			}

			if ( trust == load_trust::granted ) {
				const auto* record = options.trust->record_for( trust_scope::project,
					manifest_value->name );

				if ( record == nullptr || record->tree_sha256 != snapshot->tree_sha256 ) {
					outcome.report.inert.push_back( { manifest_value->name, candidate.directory,
						load_trust::changed,
						"the extension changed since it was granted; re-grant with "
						"'mcode ext trust " + manifest_value->name + "'" } );

					continue;
				}
			}

			const auto* entry_source = snapshot->find( "init.luau" );

			if ( entry_source == nullptr ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"no init.luau" } );

				continue;
			}

			auto host_options = lua_host_options{ };
			host_options.extension_name = manifest_value->name;
			host_options.memory_limit_bytes = options.memory_limit_bytes;
			host_options.time_limit = options.time_limit;

			// The snapshot outlives the host: the closure that reads it is stored in
			// the host, and nothing here touches the filesystem again.
			const auto loaded_files = std::make_shared< const extension_snapshot >(
				std::move( *snapshot ) );

			host_options.module_loader = [ loaded_files ]( const std::string_view module_path )
				-> std::optional< std::string > {
				if ( const auto* found = resolve_module( *loaded_files, module_path );
					found != nullptr ) {
					return *found;
				}

				return std::nullopt;
			};

			auto created = lua_host::create( std::move( host_options ) );

			if ( !created ) {
				outcome.report.failed.push_back( { candidate.name, candidate.directory,
					"could not create the VM: " + created.error( ).msg } );

				continue;
			}

			// never moved again: the surface stores a pointer to this VM.
			auto host = std::make_unique< lua_host >( std::move( *created ) );

			auto surface = std::make_unique< api_surface >( );

			if ( options.register_api ) {
				// installed before the extension runs: sealing is a one-way door.
				if ( auto registered = options.register_api(
						registration{ .host = *host, .surface = *surface,
							.providers = providers, .hooks = hooks,
							.details = *manifest_value } ); !registered ) {
					// Discarded like the sibling failure paths below. Without this,
					// whatever the registration managed to install before it failed
					// stayed in the hook and provider registries pointing at a VM
					// that the `continue` then destroys.
					discard( *host, *surface, providers, hooks );

					outcome.report.failed.push_back( { candidate.name, candidate.directory,
						"API registration failed: " + registered.error( ).msg } );

					continue;
				}
			}

			const auto entry_point = candidate.directory / "init.luau";

			if ( auto ran = host->run( *entry_source, entry_point.string( ) ); !ran ) {
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
			report_entry.version = entry.details.version;
			report_entry.description = entry.details.description;
			report_entry.tree_sha256 = loaded_files->tree_sha256;
			report_entry.trust = trust;
			report_entry.bytes_used = entry.host->bytes_allocated( );
			report_entry.tools = entry.tool_names( );

			for ( const auto& tool : entry.tool_names( ) ) {
				outcome.tool_owners.insert_or_assign( tool, entry.surface.get( ) );
			}

			outcome.report.loaded.push_back( std::move( report_entry ) );
			outcome.extensions.push_back( std::move( entry ) );
		}

		return outcome;
	}

	auto load_workspace_extensions( const std::filesystem::path& workspace,
		const std::vector< std::filesystem::path >& roots, model::provider_registry& providers,
		hook_registry& hooks, const loader_options& options ) -> load_result {
		auto effective = options;
		effective.project_root = project_extension_root( workspace );

		// A missing store is the first-run state and reads as empty, which means
		// every project extension is inert. That is the correct default.
		auto store = trust_store{ };

		if ( const auto path = trust_store::default_path( ) ) {
			auto loaded = trust_store::load( *path );

			if ( !loaded ) {
				auto outcome = load_result{ };
				outcome.report.failed.push_back( { "<trust store>", *path,
					loaded.error( ).msg } );

				return outcome;
			}

			store = std::move( *loaded );
		}

		effective.trust = &store;

		return load_extensions( roots, providers, hooks, effective );
	}

	auto default_register_api( tool_registry& registry,
		const register_context& context )
		-> std::function< status( const registration& ) > {
		// the registry is captured by reference: a copy would leave the caller's registry empty.
		return [ &registry, context ]( const registration& given ) -> status {
			return given.surface.install( api_surface::install_request{ .host = given.host,
				.registry = registry, .providers = given.providers, .hooks = given.hooks,
				.details = given.details, .skills = context.skills, .servers = context.servers,
				.config = context.config, .files = context.space, .file_reads = context.reads,
				.file_permissions = context.permissions } );
		};
	}

	auto default_roots( const std::filesystem::path& workspace )
		-> std::vector< std::filesystem::path > {
		auto roots = std::vector< std::filesystem::path >{ };

		roots.push_back( project_extension_root( workspace ) );

		if ( const auto* profile = std::getenv( "USERPROFILE" ); profile != nullptr ) {
			roots.push_back( std::filesystem::path{ profile } / ".mcode" / "extensions" );
		} else if ( const auto* home = std::getenv( "HOME" ); home != nullptr ) {
			roots.push_back( std::filesystem::path{ home } / ".mcode" / "extensions" );
		}

		return roots;
	}

	auto project_extension_root( const std::filesystem::path& workspace )
		-> std::filesystem::path {
		return workspace / ".mcode" / "extensions";
	}

	auto unload_extension( tool_registry& registry, const std::string_view owner ) -> std::size_t {
		return registry.remove_owner( owner );
	}

}
