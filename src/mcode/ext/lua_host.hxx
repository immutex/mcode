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
		};

	}

	using host_function = std::function< result< std::string >( std::string_view args_json ) >;

	struct lua_host_options {
		std::string extension_name;

		// Zero means unlimited. Enforced by the VM allocator, so it covers every
		// allocation the extension causes, not just the ones we can see.
		std::uint64_t memory_limit_bytes = 0;

		// Zero means unlimited. Enforced by the VM interrupt, which fires at loop
		// back edges and calls -- not per instruction, and never inside a host
		// function.
		std::chrono::milliseconds time_limit{ 0 };
	};

	class lua_host {
	public:
		~lua_host( );

		lua_host( lua_host&& other ) noexcept;
		auto operator=( lua_host&& other ) noexcept -> lua_host&;

		lua_host( const lua_host& ) = delete;
		auto operator=( const lua_host& ) -> lua_host& = delete;

		[[nodiscard]] static auto create( lua_host_options options = { } ) -> result< lua_host >;

		// The mcode table becomes readonly at the first call to any of the
		// execution entry points: Luau enforces readonly on every C API write
		// path, so there is no host-side bypass. Register the whole API surface
		// up front -- which is what docs/18 freezes it for.
		auto register_host_function( std::string_view name, host_function function ) -> status;
		auto set_global_string( std::string_view name, std::string_view text ) -> status;

		auto run( std::string_view chunk, std::string_view chunk_name = "=(extension)" ) -> status;
		auto run_file( const std::filesystem::path& path ) -> status;

		[[nodiscard]] auto eval_to_string( std::string_view expression ) -> result< std::string >;

		// Discards the extension thread and installs a fresh sandboxed one. The
		// VM's shared global state survives; host registrations the extension
		// made must be dropped by the caller.
		auto reset_thread( ) -> status;

		[[nodiscard]] auto sealed( ) const noexcept -> bool { return sealed_; }
		[[nodiscard]] auto valid( ) const noexcept -> bool { return state_ != nullptr; }
		[[nodiscard]] auto raw( ) noexcept -> lua_State* { return thread_; }

		[[nodiscard]] auto version_string( ) const -> std::string;
		[[nodiscard]] auto bytes_allocated( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto peak_bytes_allocated( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto memory_refusals( ) const noexcept -> std::uint64_t;
		[[nodiscard]] auto time_expired( ) const noexcept -> bool;

	private:
		lua_host( ) = default;

		auto seal( ) -> status;
		auto arm_watchdog( ) -> void;

		lua_State* state_ = nullptr;
		lua_State* thread_ = nullptr;
		int thread_index_ = 0;
		bool sealed_ = false;
		std::chrono::milliseconds time_limit_{ 0 };
		std::string extension_name_;

		std::unique_ptr< detail::allocator_state > allocator_;
		std::unique_ptr< detail::watchdog_state > watchdog_;
		std::unique_ptr< std::map< std::string, host_function, std::less<> > > host_functions_;
	};

}
