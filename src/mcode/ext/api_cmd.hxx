#pragma once

// `mcode.cmd.register`.
//
// A slash command is a presentation capability: it puts the extension's
// handler in front of the user at the REPL. It grants nothing the extension
// does not already have, so it carries no manifest permission. The registry
// lives in the ext layer because the REPL reads it from there and because
// attribution follows the same rule as every other registration.

#include <functional>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// One registered slash command.
	struct registered_command {
		std::string name;
		std::string description;
		std::string owner;

		// The handler, by registry reference in the owning VM. The same rule as
		// a tool's `run`: a Luau function is a value in its VM and cannot be
		// copied into C++.
		int function_reference = 0;

		// Whether the command declared a completion function. The reference is
		// zero when it did not.
		bool completable = false;
		int complete_reference = 0;
	};

	// The commands every loaded surface registered, in registration order.
	// The REPL resolves a typed line against this list; duplicate names across
	// extensions are refused at registration, so resolution is unambiguous.
	class command_registry {
	public:
		// Adds a command. Fails on a duplicate name: two handlers for one
		// slash command is ambiguous and the ambiguity must be reported, not
		// resolved by load order.
		auto add( registered_command command ) -> mcode::result< int >;

		// Removes every command an extension owns. Called on discard and on
		// reload, while the owning VM is still alive so the references can be
		// released by the surface that holds them.
		auto remove_owner( const std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto find( const std::string_view name ) const
			-> const registered_command*;

		[[nodiscard]] auto all( ) const noexcept -> const std::vector< registered_command >& {
			return commands_;
		}

		[[nodiscard]] auto size( ) const noexcept -> std::size_t {
			return commands_.size( );
		}

	private:
		std::vector< registered_command > commands_;
	};

	// Calls a registered command with the argument text the user typed after
	// the name. Returns the extension's first return value rendered as text;
	// a raised error is contained and reported rather than propagated.
	[[nodiscard]] auto invoke_command( api_surface& surface,
		const registered_command& command, const std::string_view arguments )
		-> mcode::result< std::string >;

	auto handle_cmd_register( lua_State* state ) -> int;

}
