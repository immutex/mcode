#include "mcode/perm/floor.hxx"

#include "mcode/perm/argv.hxx"

#include <array>
#include <cstdlib>

#include "mcode/platform/seams.hxx"

namespace mcode::perm {

	namespace {


	// The floor's exec rules, as first tokens. `sudo`, `doas`, `runas` as
	// the first token is escalation; the same word as an argument is not.
	inline constexpr auto FLOOR_PROGRAMS = std::array< std::string_view, 7 >{
		"sudo", "sudo.exe", "doas", "doas.exe", "runas", "runas.exe", "mkfs",
	};

	// mkfs family: mkfs.ext4, mkfs.ntfs, ... all share the `mkfs.` prefix
	// and there is no legitimate reason for an agent to invoke one. The
	// near-miss (`man mkfs.ext4`) has a different first token.
	inline constexpr std::string_view MKFS_PREFIX = "mkfs.";

	// Partition/format tools. `format` on Windows; the same word as an
	// argument (`man format`) has a different first token.
	inline constexpr auto FLOOR_FORMAT_PROGRAMS = std::array< std::string_view, 6 >{
		"fdisk", "fdisk.exe", "diskpart", "diskpart.exe", "format", "format.exe",
	};

	// Recursive-delete tokens that make the target a deletion target.
	inline constexpr std::string_view RM_PROGRAM = "rm";
	inline constexpr std::string_view RM_RECURSE_FORCE = "-rf";
	inline constexpr std::string_view RM_FORCE_RECURSE = "-fr";

	// `dd`'s output-file option.
	inline constexpr std::string_view DD_PROGRAM = "dd";
	inline constexpr std::string_view DD_OF_PREFIX = "of=";

	// Device-path prefixes for the dd target check.
	inline constexpr std::string_view DEV_PREFIX = "/dev/";
	inline constexpr std::string_view NVME_PREFIX = "/dev/nvme";
	inline constexpr std::string_view PHYSICALDRIVE_PREFIX = "\\\\.\\physicaldrive";

	// `~` and the env-var spellings of home. `$HOME` cannot appear in parsed
	// argv (the parser refuses `$`), so the token arrives only as a literal.
	inline constexpr std::string_view TILDE = "~";
	inline constexpr std::string_view TILDE_SLASH = "~/";
	inline constexpr std::string_view HOME_ENV = "HOME";

	[[nodiscard]] auto lower_ascii( const std::string_view text ) -> std::string {
		auto out = std::string{ text };

		for ( auto& character : out ) {
			if ( character >= 'A' && character <= 'Z' ) {
				character = static_cast< char >( character - 'A' + 'a' );
			}
		}

		return out;
	}

	[[nodiscard]] auto token_equals( const std::string_view token,
		const std::string_view expected ) -> bool {
		return lower_ascii( token ) == lower_ascii( expected );
	}

	// The first token, path-stripped and lowercased -- the same
	// normalisation is_exec_runner applies, so `/bin/sudo` and `SUDO` land
	// on the floor.
	[[nodiscard]] auto program_name( const std::vector< std::string >& argv )
		-> std::string {
		const auto& raw = argv.front( );
		const auto slash = raw.find_last_of( "/\\" );
		const auto base = slash == std::string::npos
			? raw
			: std::string_view{ raw }.substr( slash + 1 );

		return lower_ascii( base );
	}

	// True when the token names a filesystem root. Unconditional: it must not
	// depend on the home directory being resolvable, or `rm -rf /` stops
	// firing the floor on a machine with no HOME set -- and then `--yolo`
	// turns the resulting `ask` into an allow.
	[[nodiscard]] auto is_filesystem_root( const std::filesystem::path& target ) -> bool {
		return target == target.root_path( );
	}

	// True when the token names the user's home directory. The canonical form
	// is what makes `rm -rf /`, `rm -rf //` and `rm -rf /./` one command; the
	// tilde spelling resolves to the same directory and is caught by the same
	// comparison.
	[[nodiscard]] auto is_home_directory( const std::filesystem::path& target,
		const std::filesystem::path& home ) -> bool {
		return !home.empty( ) && ( target == home || target == home.root_path( ) );
	}

	// Resolves an argv token to a canonical path. Relative tokens resolve
	// against the workspace root, which is where exec runs.
	[[nodiscard]] auto resolve_target( const std::string& token,
		const mcode::workspace& space ) -> std::optional< std::filesystem::path > {
		auto candidate = std::filesystem::path{ token };

		if ( candidate.is_relative( ) ) {
			candidate = space.root( ) / candidate;
		}

		auto canonical = platform::canonicalize( candidate );

		if ( !canonical ) {
			return std::nullopt;
		}

		return *canonical;
	}

	// The home directory, canonicalized. Empty when HOME is unset -- the
	// floor's home rules then simply do not fire, which is fail-closed for
	// a rule whose subject does not exist on this machine.
	[[nodiscard]] auto home_directory( ) -> std::filesystem::path {
		const auto* value = std::getenv( "HOME" );
		const auto* alt = std::getenv( "USERPROFILE" );

		const auto raw = std::filesystem::path{
			value != nullptr && *value != '\0' ? value
			: alt != nullptr ? alt
			: "" };

		if ( raw.empty( ) ) {
			return { };
		}

		auto canonical = platform::canonicalize( raw );

		return canonical ? *canonical : std::filesystem::path{ };
	}

	// --- floor checks, one per rule ------------------------------------

	// Recursive delete of a filesystem root or the user's home.
	}

	[[nodiscard]] auto floor_recursive_delete( const std::vector< std::string >& argv,
		const mcode::workspace& space ) -> std::optional< std::string > {
		if ( !token_equals( argv.front( ), RM_PROGRAM ) ) {
			return std::nullopt;
		}

		auto recursive = false;
		auto targets = std::vector< std::string >{ };

		for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
			const auto& token = argv[ index ];

			if ( token_equals( token, RM_RECURSE_FORCE ) ||
				token_equals( token, RM_FORCE_RECURSE ) ) {
				recursive = true;

				continue;
			}

			if ( token.starts_with( '-' ) ) {
				continue;
			}

			targets.push_back( token );
		}

		if ( !recursive || targets.empty( ) ) {
			return std::nullopt;
		}

		const auto home = home_directory( );

		for ( const auto& target : targets ) {
			// A literal `~` or `~/...` resolves against the real home, not
			// the workspace: `rm -rf ~` must fire even though the shell
			// would expand it to a path outside the workspace.
			if ( target == TILDE || target.starts_with( TILDE_SLASH ) ) {
				if ( !home.empty( ) ) {
					const auto expanded = target == TILDE
						? home
						: home / std::filesystem::path{ target.substr( TILDE_SLASH.size( ) ) };

					// The floor names the home itself, not its contents:
					// `rm -rf ~/scratch/project-a` is the near-miss that
					// must stay allowed. Compare the canonical expansion
					// against the home, component-wise.
					auto canonical = platform::canonicalize( expanded );

					if ( canonical && *canonical == home ) {
						return "recursive delete of the home directory";
					}
				}

				continue;
			}

			if ( token_equals( target, HOME_ENV ) ) {
				// A literal `HOME` token is a relative path named HOME, not
				// the environment variable -- the parser already refused
				// `$HOME`. Do not treat it as the home directory.
				continue;
			}

			const auto resolved = resolve_target( target, space );

			if ( !resolved ) {
				continue;
			}

			if ( is_filesystem_root( *resolved ) ) {
				return "recursive delete of a filesystem root";
			}

			if ( is_home_directory( *resolved, home ) ) {
				return "recursive delete of the home directory";
			}
		}

		return std::nullopt;
	}

	// Privilege escalation: sudo / doas / runas as the first token.
	[[nodiscard]] auto floor_privilege_escalation(
		const std::vector< std::string >& argv ) -> std::optional< std::string > {
		const auto program = program_name( argv );

		for ( const auto candidate : FLOOR_PROGRAMS ) {
			if ( program == candidate ) {
				return "privilege escalation";
			}
		}

		return std::nullopt;
	}

	// Filesystem creation: mkfs and the mkfs.* family as the first token.
	[[nodiscard]] auto floor_filesystem_creation(
		const std::vector< std::string >& argv ) -> std::optional< std::string > {
		const auto program = program_name( argv );

		if ( program == "mkfs" || program.starts_with( MKFS_PREFIX ) ) {
			return "filesystem creation";
		}

		return std::nullopt;
	}

	// Partition/format tools as the first token.
	[[nodiscard]] auto floor_partition_tools( const std::vector< std::string >& argv )
		-> std::optional< std::string > {
		const auto program = program_name( argv );

		for ( const auto candidate : FLOOR_FORMAT_PROGRAMS ) {
			if ( program == candidate ) {
				return "partition or format tool";
			}
		}

		return std::nullopt;
	}

	// Raw write to a device: dd whose of= target is a device path.
	[[nodiscard]] auto floor_raw_device_write( const std::vector< std::string >& argv )
		-> std::optional< std::string > {
		if ( !token_equals( argv.front( ), DD_PROGRAM ) ) {
			return std::nullopt;
		}

		for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
			const auto& token = argv[ index ];

			if ( !token.starts_with( DD_OF_PREFIX ) ) {
				continue;
			}

			const auto target = lower_ascii( token.substr( DD_OF_PREFIX.size( ) ) );

			if ( target.starts_with( DEV_PREFIX ) || target.starts_with( NVME_PREFIX ) ||
				target.starts_with( PHYSICALDRIVE_PREFIX ) ) {
				return "raw write to a device";
			}
		}

		return std::nullopt;
	}



	// The floor's reason for this request, or nothing when it is not on the
	// floor. The order is fixed and the first hit wins; every rule is a
	// first-token or canonical-target check, so none of them is satisfied by a
	// near-miss.
	auto floor_reason( const request_identity& request, const mcode::workspace& space )
		-> std::optional< std::string > {
		if ( request.klass == tool_class::write ) {
			// Writing .git/ or .mcode/ internals. Matched on the canonical
			// path's first component below a boundary root, so `.gitignore` --
			// a different name -- is not caught.
			auto candidate = std::filesystem::path{ request.resource };

			if ( candidate.is_relative( ) ) {
				candidate = space.root( ) / candidate;
			}

			const auto canonical = platform::canonicalize( candidate );

			// A path that cannot be canonicalized cannot be shown to be outside
			// the protected set, so it is refused. Falling through here would
			// let an unresolvable path skip the floor entirely.
			if ( !canonical ) {
				return "write to an unresolvable path";
			}

			if ( !space.is_protected( *canonical ) ) {
				return std::nullopt;
			}

			return "write to protected harness internals";
		}

		if ( request.klass != tool_class::exec ) {
			return std::nullopt;
		}

		// The caller hands us the canonical argv as one string. Re-parsing with
		// the same parser the tool layer used keeps a quoted token containing a
		// space intact -- splitting on ' ' would turn one token into two and
		// resolve a path that was never named.
		const auto parsed = parse_command_line( request.resource );

		if ( !parsed || parsed->empty( ) ) {
			return std::nullopt;
		}

		const auto& argv = *parsed;

		if ( const auto hit = floor_privilege_escalation( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_filesystem_creation( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_partition_tools( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_raw_device_write( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_recursive_delete( argv, space ) ) {
			return hit;
		}

		return std::nullopt;
	}

}
