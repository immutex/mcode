#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mcode/fs/workspace.hxx"
#include "mcode/perm/store.hxx"

namespace mcode::perm {

	// a policy check on parsed argv and canonical paths, not a sandbox: --yolo does not bypass it.

	[[nodiscard]] auto floor_recursive_delete( const std::vector< std::string >& argv,
		const mcode::workspace& space ) -> std::optional< std::string >;

	// the same word as an argument is not escalation.
	[[nodiscard]] auto floor_privilege_escalation( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	[[nodiscard]] auto floor_filesystem_creation( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	[[nodiscard]] auto floor_partition_tools( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// `dd of=./image.img` is not a device write.
	[[nodiscard]] auto floor_raw_device_write( const std::vector< std::string >& argv )
		-> std::optional< std::string >;

	// takes the identity, not permission_request: permission.hxx includes this header.
	[[nodiscard]] auto floor_reason( const request_identity& request,
		const mcode::workspace& space ) -> std::optional< std::string >;

}
