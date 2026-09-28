#pragma once

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode {

	struct string_hash {
		using is_transparent = void;
		using is_avalanching = void;

		[[nodiscard]] auto operator( )( const std::string_view text ) const noexcept -> std::uint64_t {
			return ankerl::unordered_dense::hash< std::string_view >{}( text );
		}
	};

	enum class tool_class { read, write, exec, net, spawn };

	[[nodiscard]] auto to_string( tool_class klass ) noexcept -> std::string_view;

	enum class tool_source { core, bundled_extension, project_extension, user_extension, mcp };

	[[nodiscard]] auto to_string( tool_source source ) noexcept -> std::string_view;

	struct tool_def {
		std::string name;
		std::string description;
		tool_class klass = tool_class::read;
		tool_source source = tool_source::core;
		std::string owner;
		bool deferrable = true;

		[[nodiscard]] auto is_core( ) const noexcept -> bool {
			return source == tool_source::core;
		}
	};

	class tool_registry {
	public:
		auto add( tool_def definition ) -> status;

		[[nodiscard]] auto find( const std::string_view name ) const -> const tool_def*;

		[[nodiscard]] auto all( ) const -> std::vector< const tool_def* >;

		[[nodiscard]] auto owned_by( const std::string_view owner ) const -> std::vector< const tool_def* >;

		// Removes one tool by name. False when the name is absent, which is a
		// no-op rather than an error: unregistering something already gone is a
		// legitimate race, not a bug.
		auto remove( std::string_view name ) -> bool;

		auto remove_owner( const std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return tools_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return tools_.empty( ); }

		auto clear( ) noexcept -> void { tools_.clear( ); }

	private:
		using tool_map = ankerl::unordered_dense::map< std::string, tool_def, string_hash, std::equal_to<> >;

		tool_map tools_;
	};

}
