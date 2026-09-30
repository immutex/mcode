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

	// The entry point is undocumented but production-standard: Chrome, Firefox
	// and Nix all resolve it from libSystem at runtime. Declaring it directly
	// would need a private header; dlsym keeps the SDK contract public.
	using sandbox_init_with_parameters_fn =
		int ( * )( const char* profile, uint64_t parameters_length,
			const char* const parameters[], char** errorbuf );

	inline constexpr int SANDBOX_NAMED_EXTERNAL = 0x0003;

	// The deny-default profile. Static, not generated: a generated profile is a
	// parser, and a parser for a security policy is a vulnerability. The
	// (deny default) is mandatory -- (allow default) profiles are structurally
	// escapable -- and system.sb imports the rules every process needs to boot.
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

		// `/dev`, `/etc` and `/tmp` are symlinks into `/private`, and Seatbelt
		// matches the resolved path -- so `(subpath "/dev")` alone does not
		// cover `/dev/null`, whose real path is `/private/dev/null`. Both
		// spellings are listed because which one a caller passes is not
		// knowable here.
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

		// The .git deny is explicit: a write grant on the workspace root would
		// otherwise cover it, and the harness's own write rules treat .git as
		// protected for every actor but the harness itself.
		for ( const auto& path : profile.write_paths ) {
			text += "(deny file-write* (subpath \"" + path.string( ) + "/.git\"))\n";
			text += "(deny file-write* (subpath \"" + path.string( ) + "/.mcode\"))\n";
		}

		// Read on `/dev` is already allowed above; write is granted only on the
		// sinks, because `/dev` also holds the raw disks. A redirect to
		// `/dev/null` is not a nicety -- without it `grep x /dev/null` exits 2
		// rather than 1, because the shell cannot open the file.
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

		const auto applied = init_fn( profile_text.c_str( ), SANDBOX_NAMED_EXTERNAL,
			nullptr, &errorbuf );

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
