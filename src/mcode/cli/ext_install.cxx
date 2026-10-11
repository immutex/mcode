#include "mcode/cli/ext_command.hxx"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <system_error>

#include "mcode/cli/exec.hxx"
#include "mcode/ext/integrity.hxx"
#include "mcode/ext/lockfile.hxx"
#include "mcode/ext/manifest.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/text.hxx"

namespace mcode::cli {

	namespace {

		constexpr auto GIT_TIMEOUT = std::chrono::milliseconds{ 120'000 };

		// Removes the staging tree on every exit path. The success path renames it
		// into place first, so the destructor then finds nothing to remove.
		struct staging_guard {
			std::filesystem::path directory;

			~staging_guard( ) {
				auto ignored = std::error_code{ };
				std::filesystem::remove_all( directory, ignored );
			}
		};

		struct git_result {
			int exit_code = -1;
			std::string stdout_text;
			std::string stderr_text;
		};

		// Every git invocation is offline-safe and hook-free: a clone runs the
		// remote's repository, so anything that can execute on checkout is disabled
		// before the first byte arrives.
		auto git( const std::vector< std::string >& args,
			const std::filesystem::path& working_directory ) -> result< git_result > {
			auto options = process_options{ };
			options.executable = "git";
			options.args = args;
			options.working_directory = working_directory.string( );
			options.timeout = GIT_TIMEOUT;
			options.environment = minimal_environment( );

			const auto ran = run_process( options );

			if ( !ran ) {
				return std::unexpected( fail( errc::io,
					"cannot run git: " + ran.error( ).msg ) );
			}

			return git_result{ .exit_code = ran->exit_code,
				.stdout_text = ran->stdout_text,
				.stderr_text = ran->stderr_text };
		}

		auto trim( std::string text ) -> std::string {
			while ( !text.empty( ) && ( text.back( ) == '\n' || text.back( ) == '\r' ) ) {
				text.pop_back( );
			}

			return text;
		}

		// Resolves a ref to a commit before anything is fetched. An annotated tag
		// emits two lines and only the `^{}` line is the commit; recording the first
		// line would pin the tag object instead.
		auto resolve_ref( const std::string& url, const std::string& ref ) -> result< std::string > {
			auto staging = platform::temp_directory( );

			if ( !staging ) {
				return std::unexpected( staging.error( ) );
			}

			const auto resolved = git( { "ls-remote", url, ref }, *staging );

			if ( !resolved ) {
				return std::unexpected( resolved.error( ) );
			}

			if ( resolved->exit_code != 0 ) {
				return std::unexpected( fail( errc::config,
					"git ls-remote failed for " + ref + ": " + trim( resolved->stderr_text ) ) );
			}

			auto peeled = std::string{ };
			auto direct = std::string{ };
			auto lines = std::size_t{ 0 };

			auto stream = std::istringstream{ resolved->stdout_text };
			auto line = std::string{ };

			while ( std::getline( stream, line ) ) {
				line = trim( line );

				if ( line.empty( ) ) {
					continue;
				}

				++lines;

				const auto tab = line.find( '\t' );

				if ( tab == std::string::npos ) {
					continue;
				}

				const auto object = line.substr( 0, tab );
				const auto name = line.substr( tab + 1 );

				if ( name.ends_with( "^{}" ) ) {
					peeled = object;
				} else {
					direct = object;
				}
			}

			if ( peeled.empty( ) && direct.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"ref '" + ref + "' does not exist in " + url ) );
			}

			// More than one line and no peel means the ref is ambiguous, which is a
			// mistake to surface rather than to guess at.
			if ( peeled.empty( ) && lines > 1 ) {
				return std::unexpected( fail( errc::config,
					"ref '" + ref + "' matches more than one object in " + url ) );
			}

			return peeled.empty( ) ? direct : peeled;
		}

		auto commit_format_of( const std::string& commit ) -> std::string {
			return commit.size( ) == 64 ? "sha256" : "sha1";
		}

	}

	auto run_ext_install( const std::string& coordinate ) -> int {
		const auto at = coordinate.rfind( '@' );

		if ( at == std::string::npos || at == 0 || at + 1 >= coordinate.size( ) ) {
			std::fprintf( stderr,
				"mcode: expected git:owner/repo@<ref>, got '%s'\n", coordinate.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto source = coordinate.substr( 0, at );
		const auto ref = coordinate.substr( at + 1 );

		auto url = std::string{ };
		auto resolved_ref = std::string{ };

		if ( auto parsed = ext::parse_git_coordinate( source, ref, url, resolved_ref ); !parsed ) {
			std::fprintf( stderr, "mcode: %s\n", parsed.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto commit = resolve_ref( url, resolved_ref );

		if ( !commit ) {
			std::fprintf( stderr, "mcode: %s\n", commit.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		std::printf( "resolved %s@%s -> %s\n", source.c_str( ), resolved_ref.c_str( ),
			commit->c_str( ) );

		const auto staging_base = platform::temp_directory( );

		if ( !staging_base ) {
			std::fprintf( stderr, "mcode: %s\n", staging_base.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		auto staging = *staging_base / ( "mcode-ext-" + std::to_string( platform::current_process_id( ) ) );
		auto error = std::error_code{ };
		std::filesystem::remove_all( staging, error );

		// Outside the clone target: `git clone` needs its destination absent or
		// empty, so a hooks directory created inside it fails the clone.
		const auto hooks = *staging_base
			/ ( "mcode-ext-hooks-" + std::to_string( platform::current_process_id( ) ) );
		std::filesystem::remove_all( hooks, error );
		std::filesystem::create_directories( hooks, error );

		if ( error ) {
			std::fprintf( stderr, "mcode: cannot create %s\n", hooks.string( ).c_str( ) );

			return to_int( exit_code::usage_error );
		}

		[[maybe_unused]] auto cleanup = staging_guard{ staging };
		[[maybe_unused]] auto hooks_cleanup = staging_guard{ hooks };

		const auto cloned = git( { "clone", "--quiet", "--depth", "1", "--single-branch",
			"--no-tags", "--no-checkout",
			"-c", "core.hooksPath=" + hooks.string( ),
			"-c", "protocol.file.allow=never",
			url, staging.string( ) }, *staging_base );

		if ( !cloned ) {
			std::fprintf( stderr, "mcode: %s\n", cloned.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		if ( cloned->exit_code != 0 ) {
			std::fprintf( stderr, "mcode: clone failed: %s\n",
				trim( cloned->stderr_text ).c_str( ) );

			return to_int( exit_code::usage_error );
		}

		// A repository carrying submodules is refused, not cloned-and-ignored:
		// submodules are a code-execution path on checkout.
		if ( std::filesystem::exists( staging / ".gitmodules" ) ) {
			std::fprintf( stderr,
				"mcode: this repository declares submodules, which are refused\n" );

			return to_int( exit_code::usage_error );
		}

		const auto fetched = git( { "fetch", "--quiet", "--depth", "1", "origin", *commit },
			staging );

		if ( !fetched || fetched->exit_code != 0 ) {
			std::fprintf( stderr, "mcode: cannot fetch %s\n", commit->c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto checked_out = git( { "checkout", "--quiet", "--detach", *commit }, staging );

		if ( !checked_out || checked_out->exit_code != 0 ) {
			std::fprintf( stderr, "mcode: cannot check out %s\n", commit->c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto head = git( { "rev-parse", "HEAD" }, staging );

		if ( !head || head->exit_code != 0 || trim( head->stdout_text ) != *commit ) {
			std::fprintf( stderr,
				"mcode: the checkout is not the resolved commit; refusing to install\n" );

			return to_int( exit_code::usage_error );
		}

		std::filesystem::remove_all( staging / ".git", error );

		const auto snapshot = ext::snapshot_extension( staging );

		if ( !snapshot ) {
			std::fprintf( stderr, "mcode: %s\n", snapshot.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto* manifest_text = snapshot->find( "ext.toml" );

		if ( manifest_text == nullptr ) {
			std::fprintf( stderr, "mcode: no ext.toml at the repository root\n" );

			return to_int( exit_code::usage_error );
		}

		// The same validation the loader applies, so install-time and load-time
		// acceptance cannot drift.
		const auto name = std::filesystem::path{ source.substr( source.rfind( '/' ) + 1 ) }
			.filename( ).string( );

		const auto details = ext::parse_manifest( *manifest_text, url + "/ext.toml", name );

		if ( !details ) {
			std::fprintf( stderr, "mcode: %s\n", details.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		// User scope, per `25`: that directory is trusted by location, so an install
		// the user typed needs no grant record. Project scope is the one that arrives
		// without the user typing anything, and that is the one the trust gate holds.
		const auto user_root = platform::app_data_path( platform::data_kind::config );

		if ( !user_root ) {
			std::fprintf( stderr, "mcode: %s\n", user_root.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		const auto destination = *user_root / "extensions" / details->name;

		std::filesystem::create_directories( destination.parent_path( ), error );

		if ( error ) {
			std::fprintf( stderr, "mcode: cannot create %s\n",
				destination.parent_path( ).string( ).c_str( ) );

			return to_int( exit_code::usage_error );
		}

		std::filesystem::remove_all( destination, error );
		std::filesystem::rename( staging, destination, error );

		if ( error ) {
			std::filesystem::copy( staging, destination,
				std::filesystem::copy_options::recursive, error );

			if ( error ) {
				std::fprintf( stderr, "mcode: cannot install to %s: %s\n",
					destination.string( ).c_str( ), error.message( ).c_str( ) );

				return to_int( exit_code::usage_error );
			}

			std::filesystem::remove_all( staging, error );
		}

		const auto installed = ext::snapshot_extension( destination );

		if ( !installed ) {
			std::fprintf( stderr, "mcode: %s\n", installed.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		// The install IS the grant, but by location rather than by record: the user
		// scope is trusted by location (`19`), and the user typed this command. The
		// resolved SHA goes in the lockfile, which is the install-time record; the
		// trust store stays the load-time authority for PROJECT scope, where nothing
		// was typed.
		const auto lock_path = ext::lockfile::user_path( );

		if ( !lock_path ) {
			std::fprintf( stderr, "mcode: %s\n", lock_path.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		auto lock = ext::lockfile::load( *lock_path );

		if ( !lock ) {
			std::fprintf( stderr, "mcode: %s\n", lock.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		auto entry = ext::lock_entry{ };
		entry.name = details->name;
		entry.source = source;
		entry.ref = resolved_ref;
		entry.commit = *commit;
		entry.commit_format = commit_format_of( *commit );
		entry.tree_sha256 = installed->tree_sha256;
		entry.api_version = details->api_version;
		entry.permissions = details->permissions;

		lock->put( entry );

		if ( const auto saved = lock->save( ); !saved ) {
			std::fprintf( stderr, "mcode: %s\n", saved.error( ).msg.c_str( ) );

			return to_int( exit_code::usage_error );
		}

		std::printf( "installed %s %s\n", details->name.c_str( ), details->version.c_str( ) );
		std::printf( "  commit:     %s (%s)\n", commit->c_str( ),
			commit_format_of( *commit ).c_str( ) );
		std::printf( "  tree:       %s\n", installed->tree_sha256.c_str( ) );
		std::printf( "  location:   %s\n", destination.string( ).c_str( ) );

		if ( !details->permissions.empty( ) ) {
			std::printf( "  permissions: " );

			for ( const auto& permission : details->permissions ) {
				std::printf( "%s ", permission.c_str( ) );
			}

			std::printf( "\n" );
		}

		const auto suspicious = text::count_invisible( destination );

		if ( suspicious > 0 ) {
			std::printf( "  warning: %zu invisible or confusable codepoints in the source; "
				"read it before trusting it\n", suspicious );
		}

		return to_int( exit_code::success );
	}

}
