#include "mcode/platform/sandbox_linux.hxx"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined( __linux__ )
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mcode::platform {

#if defined( __linux__ )

	// glibc ships no wrappers for the Landlock trio; the kernel ABI is the contract.
	inline constexpr long SYSCALL_LANDLOCK_CREATE_RULESET = __NR_landlock_create_ruleset;
	inline constexpr long SYSCALL_LANDLOCK_ADD_RULE = __NR_landlock_add_rule;
	inline constexpr long SYSCALL_LANDLOCK_RESTRICT_SELF = __NR_landlock_restrict_self;

	// granted read+execute, never write; a missing path (e.g. /lib64) is skipped.
	inline constexpr std::string_view SYSTEM_READ_ROOTS[] = {
		"/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/dev",
	};

	// ABI 5's IOCTL_DEV right: an enum member, so #ifdef cannot detect a missing one.
	inline constexpr std::uint64_t LANDLOCK_ACCESS_FS_IOCTL_DEV_RIGHT = 1ULL << 15;

	// accumulated up the ladder; ABI 1 has no REFER, so cross-directory rename/link are denied.
	[[nodiscard]] auto landlock_fs_rights( const int abi ) -> std::uint64_t {
		auto rights = std::uint64_t{ 0 };

		rights |= LANDLOCK_ACCESS_FS_EXECUTE;
		rights |= LANDLOCK_ACCESS_FS_WRITE_FILE;
		rights |= LANDLOCK_ACCESS_FS_READ_FILE;
		rights |= LANDLOCK_ACCESS_FS_READ_DIR;
		rights |= LANDLOCK_ACCESS_FS_REMOVE_DIR;
		rights |= LANDLOCK_ACCESS_FS_REMOVE_FILE;
		rights |= LANDLOCK_ACCESS_FS_MAKE_CHAR;
		rights |= LANDLOCK_ACCESS_FS_MAKE_DIR;
		rights |= LANDLOCK_ACCESS_FS_MAKE_REG;
		rights |= LANDLOCK_ACCESS_FS_MAKE_SOCK;
		rights |= LANDLOCK_ACCESS_FS_MAKE_FIFO;
		rights |= LANDLOCK_ACCESS_FS_MAKE_BLOCK;
		rights |= LANDLOCK_ACCESS_FS_MAKE_SYM;

		if ( abi >= 2 ) {
			rights |= LANDLOCK_ACCESS_FS_REFER;
		}

		if ( abi >= 3 ) {
			rights |= LANDLOCK_ACCESS_FS_TRUNCATE;
		}

		if ( abi >= 5 ) {
			rights |= LANDLOCK_ACCESS_FS_IOCTL_DEV_RIGHT;
		}

		return rights;
	}

	[[nodiscard]] auto landlock_net_rights( const int abi ) -> std::uint64_t {
		if ( abi < 4 ) {
			return 0;
		}

		return LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
	}

#endif

	auto sandbox_linux_abi( ) -> result< int > {
#if defined( __linux__ )
		const auto version = ::syscall( SYSCALL_LANDLOCK_CREATE_RULESET, nullptr, 0,
			LANDLOCK_CREATE_RULESET_VERSION );

		if ( version < 0 ) {
			return std::unexpected( mcode::fail( mcode::errc::unsupported,
				std::string{ "landlock_create_ruleset(VERSION) failed: " } +
				std::strerror( errno ) +
				"; the kernel does not expose Landlock" ) );
		}

		return static_cast< int >( version );
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"Landlock exists only on Linux" ) );
#endif
	}

	auto sandbox_linux_ruleset( const int abi, const sandbox_profile& profile )
		-> result< unique_ruleset_linux > {
#if !defined( __linux__ )
		(void)abi;
		(void)profile;
#endif

#if defined( __linux__ )
		if ( abi < 1 ) {
			return std::unexpected( mcode::fail( mcode::errc::unsupported,
				"no Landlock ABI on this kernel" ) );
		}

		auto handled_fs = landlock_fs_rights( abi );
		auto handled_net = landlock_net_rights( abi );

		auto attr = landlock_ruleset_attr{ };
		attr.handled_access_fs = handled_fs;
		attr.handled_access_net = profile.allow_network ? std::uint64_t{ 0 } : handled_net;

		const auto ruleset_fd = ::syscall( SYSCALL_LANDLOCK_CREATE_RULESET, &attr,
			sizeof( attr ), 0U );

		if ( ruleset_fd < 0 ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				std::string{ "landlock_create_ruleset failed: " } + std::strerror( errno ) ) );
		}

		auto ruleset = unique_ruleset_linux{ static_cast< int >( ruleset_fd ) };

		auto grant = [ & ]( const std::filesystem::path& path,
			const std::uint64_t access ) -> status {
			const auto fd = ::open( path.c_str( ), O_PATH | O_CLOEXEC );

			if ( fd < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"cannot open " + path.string( ) + ": " + std::strerror( errno ) ) );
			}

			auto beneath = landlock_path_beneath_attr{ };
			beneath.allowed_access = handled_fs & access;
			beneath.parent_fd = fd;

			const auto added = ::syscall( SYSCALL_LANDLOCK_ADD_RULE, ruleset.get( ),
				LANDLOCK_RULE_PATH_BENEATH, &beneath, 0U );
			// Read before `close`, which may clobber it. Named for the syscall that set
			// it: `open_error` named the wrong call and sent a reader to the `open` above.
			const auto rule_error = errno;

			::close( fd );

			if ( added < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"landlock_add_rule(" + path.string( ) + ") failed: " +
					std::strerror( rule_error ) ) );
			}

			return { };
		};

		const auto read_only = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
			LANDLOCK_ACCESS_FS_READ_DIR;

		for ( const auto root : SYSTEM_READ_ROOTS ) {
			const auto path = std::filesystem::path{ root };

			if ( !std::filesystem::exists( path ) ) {
				continue;
			}

			if ( const auto granted = grant( path, read_only ); !granted ) {
				return std::unexpected( granted.error( ) );
			}
		}

		for ( const auto& path : profile.read_paths ) {
			if ( const auto granted = grant( path, read_only ); !granted ) {
				return std::unexpected( granted.error( ) );
			}
		}

		for ( const auto& path : profile.write_paths ) {
			const auto fd = ::open( path.c_str( ), O_PATH | O_CLOEXEC );

			if ( fd < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"cannot open write path " + path.string( ) + ": " + std::strerror( errno ) ) );
			}

			auto beneath = landlock_path_beneath_attr{ };
			beneath.allowed_access = handled_fs;
			beneath.parent_fd = fd;

			const auto added = ::syscall( SYSCALL_LANDLOCK_ADD_RULE, ruleset.get( ),
				LANDLOCK_RULE_PATH_BENEATH, &beneath, 0U );
			// Read before `close`, which may clobber it. Named for the syscall that set
			// it: `open_error` named the wrong call and sent a reader to the `open` above.
			const auto rule_error = errno;

			::close( fd );

			if ( added < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"landlock_add_rule(write " + path.string( ) + ") failed: " +
					std::strerror( rule_error ) ) );
			}
		}

		// handled with no port rule granted, so handled implies denied.
		return ruleset;
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"the Landlock ruleset exists only on Linux" ) );
#endif
	}

	auto sandbox_linux_restrict( const int abi, const unique_ruleset_linux& ruleset,
		const sandbox_profile& profile ) -> status {
#if !defined( __linux__ )
		(void)abi;
		(void)ruleset;
		(void)profile;
#endif

#if defined( __linux__ )
		// without no_new_privs the kernel rejects restrict_self for an unprivileged caller.
		if ( ::prctl( PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0 ) != 0 ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				std::string{ "prctl(PR_SET_NO_NEW_PRIVS) failed: " } + std::strerror( errno ) ) );
		}

		if ( ::syscall( SYSCALL_LANDLOCK_RESTRICT_SELF, ruleset.get( ), 0U ) != 0 ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				std::string{ "landlock_restrict_self failed: " } + std::strerror( errno ) ) );
		}

		if ( !profile.allow_network && abi < 4 ) {
			return sandbox_linux_seccomp_deny_network( );
		}

		return { };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"Landlock restriction exists only on Linux" ) );
#endif
	}

	auto sandbox_linux_seccomp_deny_network( ) -> status {
#if defined( __linux__ )
		auto program = std::vector< sock_filter >{
			BPF_STMT( BPF_LD | BPF_W | BPF_ABS, offsetof( seccomp_data, nr ) ),
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ALLOW ),
		};

		auto fprog = sock_fprog{ };
		fprog.len = static_cast< unsigned short >( program.size( ) );
		fprog.filter = program.data( );

		// no TSYNC: the caller is single-threaded at spawn, and the filter still covers children.
		if ( ::syscall( __NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog ) != 0 ) {
			return std::unexpected( mcode::fail( mcode::errc::io,
				std::string{ "seccomp(SECCOMP_SET_MODE_FILTER) failed: " } +
				std::strerror( errno ) ) );
		}

		return { };
#else
		return std::unexpected( mcode::fail( mcode::errc::unsupported,
			"seccomp exists only on Linux" ) );
#endif
	}

}
