#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mcode/fs/workspace.hxx"
#include "mcode/perm/store.hxx"

namespace mcode::perm {

	// The hard-deny floor: the handful of actions that are refused rather than
	// asked about, and that `--yolo` does not bypass.
	//
	// This is a policy check on parsed argv and canonical paths, NOT a sandbox.
	// A command that reaches outside those two inputs -- an opaque script, a
	// binary that deletes on its own -- is not caught here. That is what the OS
	// sandbox is for, and it does not exist yet.
	//
	// Every rule is small on purpose. A false positive is worse than a missing
	// rule: it blocks legitimate work, and a user who hits one reaches for the
	// override and then leaves it off. Each rule names what it prevents and is
	// tested against its legitimate near-miss.

	// Recursive delete whose canonical target is a filesystem root or the
	// user's home. The canonical form is what makes `rm -rf /`, `rm -rf //`
	// and `rm -rf /./` one command; a string compare misses two of them.
	[[nodiscard]] auto floor_recursive_delete( const std::vector< std::string >& argv,
		const mcode::workspace& space ) -> std::optional< std::string >;

	// `sudo`, `doas`, `runas` as the first token. The same word as an argument
	// is not escalation.
	[[nodiscard]] auto floor_privilege_escalation( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// `mkfs` and the `mkfs.*` family as the first token. `man mkfs.ext4` has a
	// different first token and is not caught.
	[[nodiscard]] auto floor_filesystem_creation( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// Partition and format tools as the first token.
	[[nodiscard]] auto floor_partition_tools( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// `dd` whose `of=` target is a device path. `dd of=./image.img` is not.
	[[nodiscard]] auto floor_raw_device_write( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// The floor's reason for refusing this request, or nothing when it is not
	// on the floor. Takes the identity rather than `permission_request` because
	// permission.hxx includes this header, and the rule set is keyed on class
	// as well as on the argv.
	[[nodiscard]] auto floor_reason( const request_identity& request,
		const mcode::workspace& space ) -> std::optional< std::string >;

}
