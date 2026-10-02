#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

struct lua_State;

typedef int ( *lua_CFunction )( lua_State* );

namespace mcode {

	namespace detail {

		struct allocator_state {
			std::uint64_t bytes = 0;
			std::uint64_t peak = 0;
			std::uint64_t limit = 0;
			std::uint64_t refusals = 0;
		};

		struct watchdog_state {
			std::chrono::steady_clock::time_point deadline{ };
			bool armed = false;
			bool expired = false;
			std::uint64_t breaches = 0;
		};

	}

	using host_function = std::function< result< std::string >( std::string_view args_json ) >;

	using module_loader_function =
		std::function< std::optional< std::string >( std::string_view module_path ) >;

	struct lua_host_options {
		std::string extension_name;

		// zero means unlimited; enforced in the VM allocator, so it covers every allocation.
		std::uint64_t memory_limit_bytes = 0;

		// zero means unlimited; the interrupt only fires at loop back edges and calls.
		std::chrono::milliseconds time_limit{ 0 };

		// an empty optional is a load error; never falls back to the filesystem at large.
		module_loader_function module_loader;
	};

	// one VM per extension: the allocator and watchdog are per-VM, so budgets are attributable.
	class lua_host {
	public:
		~lua_host( );

		lua_host( lua_host&& other ) noexcept;
		auto operator=( lua_host&& other ) noexcept -> lua_host&;

		lua_host( const lua_host& ) = delete;
		auto operator=( const lua_host& ) -> lua_host& = delete;

		[[nodiscard]] static auto create( lua_host_options options = { } ) -> result< lua_host >;

		// sealing is one-way: Luau enforces readonly on every C API write path.
		auto register_host_function( std::string_view path, host_function function ) -> status;
		auto set_global_string( std::string_view path, std::string_view text ) -> status;

		// an integer, not a string: Luau raises on a number/string comparison rather than coercing.
		auto set_global_number( std::string_view path, double value ) -> status;

		// the JSON path cannot carry a function; upvalue 1 is the owner object.
		auto register_raw_function( std::string_view path, lua_CFunction function,
			void* upvalue ) -> status;

		auto run( std::string_view chunk, std::string_view chunk_name = "=(extension)" ) -> status;
		auto run_file( const std::filesystem::path& path ) -> status;

		[[nodiscard]] auto eval_to_string( std::string_view expression ) -> result< std::string >;

		[[nodiscard]] auto call_global( std::string_view name, std::string_view argument )
			-> result< std::string >;

		// the extension's own globals survive, so the caller must also drop what it registered.
		auto reset( ) -> status;

		// disarms on exit: a stale deadline would fire at the next unrelated safepoint.
		class budget_scope {
		public:
			explicit budget_scope( lua_host* host ) noexcept;
			~budget_scope( );

			budget_scope( const budget_scope& ) = delete;
			auto operator=( const budget_scope& ) -> budget_scope& = delete;
			budget_scope( budget_scope&& ) = delete;
			auto operator=( budget_scope&& ) -> budget_scope& = delete;

		private:
			lua_host* host_ = nullptr;
		};

		[[nodiscard]] auto sealed( ) const noexcept -> bool { return sealed_; }
		[[nodiscard]] auto valid( ) const noexcept -> bool { return thread_ != nullptr; }
		[[nodiscard]] auto raw( ) noexcept -> lua_State* { return thread_; }
		[[nodiscard]] auto name( ) const noexcept -> std::string_view { return extension_name_; }

		[[nodiscard]] auto version_string( ) const -> std::string;
		[[nodiscard]] auto bytes_allocated( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto peak_bytes_allocated( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto memory_refusals( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto time_breaches( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto time_expired( ) const noexcept -> bool;

	private:
		lua_host( ) = default;

		auto seal( ) -> status;
		auto arm_watchdog( ) -> void;
		auto disarm_watchdog( ) -> void;

		auto push_namespace( std::string_view path ) -> status;

		lua_State* state_ = nullptr;
		lua_State* thread_ = nullptr;
		bool sealed_ = false;
		std::chrono::milliseconds time_limit_{ 0 };
		std::string extension_name_;

		std::unique_ptr< detail::allocator_state > allocator_;
		std::unique_ptr< detail::watchdog_state > watchdog_;
		std::unique_ptr< std::map< std::string, host_function, std::less<> > > host_functions_;
		// unique_ptr because the address goes into the Lua registry and moving must not move it.
		std::unique_ptr< module_loader_function > module_loader_;
	};

}
