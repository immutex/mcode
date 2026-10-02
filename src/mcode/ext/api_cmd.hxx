#pragma once

// a slash command is presentation, not capability, so it carries no manifest permission.

#include <functional>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	struct registered_command {
		std::string name;
		std::string description;
		std::string owner;

		int function_reference = 0;

		bool completable = false;
		int complete_reference = 0;
	};

	class command_registry {
	public:
		auto add( registered_command command ) -> mcode::result< int >;

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

	[[nodiscard]] auto invoke_command( api_surface& surface,
		const registered_command& command, const std::string_view arguments )
		-> mcode::result< std::string >;

	auto handle_cmd_register( lua_State* state ) -> int;

}
