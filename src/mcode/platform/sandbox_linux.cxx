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

	// The syscall numbers: glibc ships no wrappers for the Landlock trio, and
	// the kernel ABI is the contract, not the libc.
	inline constexpr long SYSCALL_LANDLOCK_CREATE_RULESET = __NR_landlock_create_ruleset;
	inline constexpr long SYSCALL_LANDLOCK_ADD_RULE = __NR_landlock_add_rule;
	inline constexpr long SYSCALL_LANDLOCK_RESTRICT_SELF = __NR_landlock_restrict_self;

	// The system roots every process needs to load and run: the shell, the
	// dynamic loader, libc, and the nsswitch/ld.so configuration that name
	// them. Without these a restricted child cannot exec anything at all --
	// `LANDLOCK_ACCESS_FS_EXECUTE` is handled, so an ungranted `/bin/sh` is
	// denied and the spawn fails rather than the command.
	//
	// Granted read+execute, never write: this is what makes the sandbox
	// usable, and it grants nothing the child could not already read as the
	// invoking user. A path that does not exist is skipped, because the
	// distributions disagree about `/lib64` and a missing one must not fail
	// the spawn.
	inline constexpr std::string_view SYSTEM_READ_ROOTS[] = {
		"/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/dev",
	};

	// LANDLOCK_ACCESS_FS_IOCTL_DEV, which ABI 5 introduced and which the build
	// headers may not name: they are enum members in linux/landlock.h, not
	// macros, so a missing one cannot be detected with #ifdef. The value is
	// fixed by the kernel ABI, and the ABI itself is probed at runtime.
	inline constexpr std::uint64_t LANDLOCK_ACCESS_FS_IOCTL_DEV_RIGHT = 1ULL << 15;

	// Filesystem rights per ABI, accumulated up the ladder. ABI 1 has no
	// REFER, which means every cross-directory rename and link is denied
	// under the ruleset; that is the documented cost of the oldest floor.
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

		// Denying network is only meaningful when the ABI can handle it; below
		// ABI 4 the seccomp fallback covers it, and the capability reporting
		// says which one applies.
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

		// The access mask is a parameter because a rule for a file takes the
		// same shape as one for a directory: only the granted bits differ.
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
			const auto open_error = errno;
			::close( fd );

			if ( added < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"landlock_add_rule(" + path.string( ) + ") failed: " +
					std::strerror( open_error ) ) );
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
			const auto open_error = errno;
			::close( fd );

			if ( added < 0 ) {
				return std::unexpected( mcode::fail( mcode::errc::io,
					"landlock_add_rule(write " + path.string( ) + ") failed: " +
					std::strerror( open_error ) ) );
			}
		}

		// allow_network = false with a network-capable ABI means the ruleset
		// handles the net rights and grants none: connect and bind on TCP are
		// denied. The profile grants no ports, so handled implies denied.
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
		// no_new_privs before restrict_self: without it the kernel rejects the
		// restriction for an unprivileged caller, and the call that looks like
		// a sandbox is a no-op.
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
		// BPF_STMT/BPF_LD etc. come from linux/filter.h; the filter denies the
		// syscalls that open outbound sockets. socket(2) itself is denied too:
		// a socket that cannot be created cannot connect.
		auto program = std::vector< sock_filter >{
			// Load the syscall number.
			BPF_STMT( BPF_LD | BPF_W | BPF_ABS, offsetof( seccomp_data, nr ) ),
			// socket
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			// connect
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			// socketpair
			BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1 ),
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ( EPERM & SECCOMP_RET_DATA ) ),
			// Everything else passes.
			BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ALLOW ),
		};

		auto fprog = sock_fprog{ };
		fprog.len = static_cast< unsigned short >( program.size( ) );
		fprog.filter = program.data( );

		// TSYNC is not required: the caller is single-threaded at spawn time in
		// the child path this serves, and a filter without TSYNC still applies
		// to this thread and everything it creates.
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
