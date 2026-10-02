#include "mcode/perm/floor.hxx"

#include "mcode/perm/argv.hxx"

#include <array>
#include <cstdlib>

#include "mcode/platform/seams.hxx"

namespace mcode::perm {

	namespace {

	// the floor's exec rules, as first tokens; the same word as an argument is not escalation.
	inline constexpr auto FLOOR_PROGRAMS = std::array< std::string_view, 7 >{
		"sudo", "sudo.exe", "doas", "doas.exe", "runas", "runas.exe", "mkfs",
	};

	inline constexpr std::string_view MKFS_PREFIX = "mkfs.";

	inline constexpr auto FLOOR_FORMAT_PROGRAMS = std::array< std::string_view, 6 >{
		"fdisk", "fdisk.exe", "diskpart", "diskpart.exe", "format", "format.exe",
	};

	inline constexpr std::string_view RM_PROGRAM = "rm";
	inline constexpr std::string_view RM_RECURSE_FORCE = "-rf";
	inline constexpr std::string_view RM_FORCE_RECURSE = "-fr";

	inline constexpr std::string_view DD_PROGRAM = "dd";
	inline constexpr std::string_view DD_OF_PREFIX = "of=";

	inline constexpr std::string_view DEV_PREFIX = "/dev/";
	inline constexpr std::string_view NVME_PREFIX = "/dev/nvme";
	inline constexpr std::string_view PHYSICALDRIVE_PREFIX = "\\\\.\\physicaldrive";

	// `$HOME` cannot appear in parsed argv (the parser refuses `$`), so only the literal arrives.
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

	// path-stripped and lowercased, so `/bin/sudo` and `SUDO` land on the floor.
	[[nodiscard]] auto program_name( const std::vector< std::string >& argv )
		-> std::string {
		const auto& raw = argv.front( );
		const auto slash = raw.find_last_of( "/\\" );
		const auto base = slash == std::string::npos
			? raw
			: std::string_view{ raw }.substr( slash + 1 );

		return lower_ascii( base );
	}

	// unconditional, so `rm -rf /` still fires with no HOME set and --yolo cannot allow it.
	[[nodiscard]] auto is_filesystem_root( const std::filesystem::path& target ) -> bool {
		return target == target.root_path( );
	}

	[[nodiscard]] auto is_home_directory( const std::filesystem::path& target,
		const std::filesystem::path& home ) -> bool {
		return !home.empty( ) && ( target == home || target == home.root_path( ) );
	}

	// relative tokens resolve against the workspace root, which is where exec runs.
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

	// empty when HOME and USERPROFILE are unset, so the home rules simply do not fire.
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
			// a literal `~` resolves against the real home, not the workspace.
			if ( target == TILDE || target.starts_with( TILDE_SLASH ) ) {
				if ( !home.empty( ) ) {
					const auto expanded = target == TILDE
						? home
						: home / std::filesystem::path{ target.substr( TILDE_SLASH.size( ) ) };

					// the floor names the home itself, not its contents.
					auto canonical = platform::canonicalize( expanded );

					if ( canonical && *canonical == home ) {
						return "recursive delete of the home directory";
					}
				}

				continue;
			}

			if ( token_equals( target, HOME_ENV ) ) {
				// a literal `HOME` is a path named HOME, not the environment variable.
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

	[[nodiscard]] auto floor_filesystem_creation(
		const std::vector< std::string >& argv ) -> std::optional< std::string > {
		const auto program = program_name( argv );

		if ( program == "mkfs" || program.starts_with( MKFS_PREFIX ) ) {
			return "filesystem creation";
		}

		return std::nullopt;
	}

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

	auto floor_reason( const request_identity& request, const mcode::workspace& space )
		-> std::optional< std::string > {
		if ( request.klass == tool_class::write ) {
			auto candidate = std::filesystem::path{ request.resource };

			if ( candidate.is_relative( ) ) {
				candidate = space.root( ) / candidate;
			}

			const auto canonical = platform::canonicalize( candidate );

			// an unresolvable path cannot be shown to be outside the protected set.
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

		// re-parsed with the same parser, so a quoted token containing a space stays one token.
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
