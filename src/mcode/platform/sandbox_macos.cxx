#include "mcode/platform/sandbox_macos.hxx"

#include <cstring>
#include <string>
#include <vector>

#if defined( __APPLE__ )
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

namespace mcode::platform {

#if defined( __APPLE__ )

	// undocumented but production-standard; dlsym keeps the SDK contract public.
	using sandbox_init_with_parameters_fn =
		int ( * )( const char* profile, uint64_t parameters_length,
			const char* const parameters[], char** errorbuf );

	// deny-default and static; (allow default) profiles are structurally escapable.
	auto seatbelt_profile( const sandbox_profile& profile,
		const std::filesystem::path& temp_dir ) -> std::string {
		auto text = std::string{ "(version 1)\n" };
		text += "(deny default)\n";
		text += "(import \"system.sb\")\n";

		text += "(allow process-exec* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\")"
			" (literal \"/usr/libexec/trustd\"))\n";
		text += "(allow process-fork)\n";
		text += "(allow signal (target self))\n";
		text += "(allow sysctl-read)\n";
		text += "(allow mach-lookup)\n";

		// Seatbelt matches the resolved path, and /dev and /etc are symlinks into /private.
		text += "(allow file-read* (subpath \"/System\") (subpath \"/usr\") (subpath \"/bin\")"
			" (subpath \"/sbin\") (subpath \"/dev\") (subpath \"/private/dev\")"
			" (subpath \"/private/etc\"))\n";

		for ( const auto& path : profile.read_paths ) {
			text += "(allow file-read* (subpath \"" + path.string( ) + "\"))\n";
		}

		for ( const auto& path : profile.write_paths ) {
			text += "(allow file-write* (subpath \"" + path.string( ) + "\"))\n";
			text += "(allow file-read* (subpath \"" + path.string( ) + "\"))\n";
		}

		if ( !temp_dir.empty( ) ) {
			text += "(allow file-write* (subpath \"" + temp_dir.string( ) + "\"))\n";
			text += "(allow file-read* (subpath \"" + temp_dir.string( ) + "\"))\n";
		}

		// A write grant on the workspace root would otherwise cover these.
		for ( const auto& path : profile.write_paths ) {
			text += "(deny file-write* (subpath \"" + path.string( ) + "/.git\"))\n";
			text += "(deny file-write* (subpath \"" + path.string( ) + "/.mcode\"))\n";
		}

		// write only on the sinks, because /dev also holds the raw disks.
		text += "(allow file-write* (literal \"/dev/null\") (literal \"/private/dev/null\")"
			" (literal \"/dev/zero\") (literal \"/private/dev/zero\"))\n";

		if ( !profile.allow_network ) {
			text += "(deny network*)\n";
		} else {
			text += "(allow network*)\n";
		}

		return text;
	}

#endif

	auto sandbox_macos_init( const sandbox_profile& profile,
		const std::filesystem::path& temp_dir ) -> status {
#if !defined( __APPLE__ )
		(void)profile;
		(void)temp_dir;
#endif

#if defined( __APPLE__ )
		void* system_lib = ::dlopen( nullptr, RTLD_NOW );

		if ( system_lib == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				std::string{ "dlopen(self) failed: " } + ::dlerror( ) ) );
		}

		const auto init_fn = reinterpret_cast< sandbox_init_with_parameters_fn >(
			::dlsym( RTLD_DEFAULT, "sandbox_init_with_parameters" ) );

		if ( init_fn == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::unsupported,
				std::string{ "sandbox_init_with_parameters is unavailable: " } +
				::dlerror( ) ) );
		}

		const auto profile_text = seatbelt_profile( profile, temp_dir );
		char* errorbuf = nullptr;

		// flags MUST be zero: otherwise sandbox_init reads the profile text as a filename.
		const auto applied = init_fn( profile_text.c_str( ), 0, nullptr, &errorbuf );

		if ( applied != 0 ) {
			auto message = std::string{ "sandbox_init_with_parameters failed" };

			if ( errorbuf != nullptr ) {
				message += ": ";
				message += errorbuf;
				::free( errorbuf );
			}

			return std::unexpected( mcode::fail( mcode::errc::io, message ) );
		}

		return { };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"Seatbelt exists only on macOS" ) );
#endif
	}

}
