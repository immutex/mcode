#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode {

	namespace detail {

		struct hook_state {
			std::uint64_t counter = 0;
			std::uint64_t budget = 0;
		};

	}

	using host_function = std::function< result< std::string >( std::string_view args_json ) >;

	struct lua_host_options {
		bool enable_jit = false;
		std::uint64_t instruction_budget = 0;
		std::string extension_name;
	};

	class lua_host {
	public:
		~lua_host( );

		lua_host( lua_host&& other ) noexcept;
		auto operator=( lua_host&& other ) noexcept -> lua_host&;

		lua_host( const lua_host& ) = delete;
		auto operator=( const lua_host& ) -> lua_host& = delete;

		[[nodiscard]] static auto create( lua_host_options options = { } ) -> result< lua_host >;

		auto run( std::string_view chunk, std::string_view chunk_name = "=(extension)" ) -> status;
		auto run_file( const std::filesystem::path& path ) -> status;

		[[nodiscard]] auto eval_to_string( std::string_view expression ) -> result< std::string >;

		auto register_host_function( std::string_view name, host_function function ) -> status;
		auto set_global_string( std::string_view name, std::string_view text ) -> status;

		[[nodiscard]] auto valid( ) const noexcept -> bool { return state_ != nullptr; }
		[[nodiscard]] auto raw( ) noexcept -> lua_State* { return state_; }

		[[nodiscard]] auto version_string( ) const -> std::string;
		[[nodiscard]] auto jit_enabled( ) const noexcept -> bool { return jit_enabled_; }
		[[nodiscard]] auto instructions_executed( ) const noexcept -> std::uint64_t;

	private:
		lua_host( ) = default;

		lua_State* state_ = nullptr;
		bool jit_enabled_ = false;
		std::string extension_name_;
		std::unique_ptr< detail::hook_state > hook_;
		std::unique_ptr< std::map< std::string, host_function, std::less<> > > host_functions_;
	};

}
