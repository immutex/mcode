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

	// Resolves a `require` path against the extension root and returns its source.
	// An empty optional is a load error.
	using module_loader_function =
		std::function< std::optional< std::string >( std::string_view module_path ) >;

	struct lua_host_options {
		std::string extension_name;

		// Zero means unlimited. Enforced by the VM allocator, so it covers every
		// allocation the extension causes, not just the ones we can see.
		std::uint64_t memory_limit_bytes = 0;

		// Zero means unlimited. Enforced by the VM interrupt, which fires at loop
		// back edges and calls -- not per instruction, and never inside a host
		// function.
		std::chrono::milliseconds time_limit{ 0 };

		// Resolves a require path against the extension root. Returning an empty
		// optional is a load error; the host never falls back to the filesystem
		// at large.
		module_loader_function module_loader;
	};

	// One VM per extension. Two extensions share no mutable state: not globals,
	// not the allocator, not the watchdog. That is what makes the memory ceiling
	// and the time budget attributable rather than aggregate.
	class lua_host {
	public:
		~lua_host( );

		lua_host( lua_host&& other ) noexcept;
		auto operator=( lua_host&& other ) noexcept -> lua_host&;

		lua_host( const lua_host& ) = delete;
		auto operator=( const lua_host& ) -> lua_host& = delete;

		[[nodiscard]] static auto create( lua_host_options options = { } ) -> result< lua_host >;

		// The mcode table becomes readonly at the first call to any execution
		// entry point: Luau enforces readonly on every C API write path, so there
		// is no host-side bypass. Register the whole API surface up front.
		// Two-level namespacing is frozen: `mcode.tool.register`, not a
		// flat `mcode.tool_register`. The path is created before sealing, because
		// sealing makes the surface readonly and Luau enforces readonly on every
		// C API write path.
		auto register_host_function( std::string_view path, host_function function ) -> status;
		auto set_global_string( std::string_view path, std::string_view text ) -> status;

		// `mcode.api_version` is an integer in the frozen surface, and a
		// string would break every `mcode.api_version < 2` comparison an author
		// writes. Luau compares a number to a string by raising, not by coercing.
		auto set_global_number( std::string_view path, double value ) -> status;

		// Registers a raw C closure with one lightuserdata upvalue.
		//
		// `register_host_function` marshals arguments through JSON, which cannot
		// carry a function. `mcode.tool.register(def)` receives a table containing a
		// `run` closure, so that entry point needs the C stack directly. The
		// upvalue is the owner object, retrieved with `lua_upvalueindex( 1 )`.
		auto register_raw_function( std::string_view path, lua_CFunction function,
			void* upvalue ) -> status;

		auto run( std::string_view chunk, std::string_view chunk_name = "=(extension)" ) -> status;
		auto run_file( const std::filesystem::path& path ) -> status;

		[[nodiscard]] auto eval_to_string( std::string_view expression ) -> result< std::string >;

		// Calls a Lua function by name with one string argument. Used for hook
		// dispatch; returns the raw result string.
		[[nodiscard]] auto call_global( std::string_view name, std::string_view argument )
			-> result< std::string >;

		// Discards the thread's stack and call frames. The extension's own globals
		// survive, so the caller must also drop whatever the extension registered
		// with the host.
		auto reset( ) -> status;

		// Arms the time budget for its lifetime and disarms it on exit.
		//
		// Both halves matter, and the asymmetry is why this is a scope rather than
		// two calls. Not arming lets an extension spin forever on a dispatch path
		// that never went through `run`/`eval_to_string`/`call_global` -- hooks and
		// tool calls are exactly those paths. Not DISarming leaves a deadline in the
		// past, so the first safepoint of the next dispatch raises a time-budget
		// error belonging to a call that already finished.
		class budget_scope {
		public:
			// A null host is accepted: dispatch may hold a subscription whose host
			// has gone away, and the guard must be usable on that path.
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

		// Walks a dotted path, creating intermediate tables, and leaves the parent
		// table on the stack. Shared by both registration entry points so a
		// function and a value cannot disagree about the namespace shape.
		auto push_namespace( std::string_view path ) -> status;

		lua_State* state_ = nullptr;
		lua_State* thread_ = nullptr;
		bool sealed_ = false;
		std::chrono::milliseconds time_limit_{ 0 };
		std::string extension_name_;

		std::unique_ptr< detail::allocator_state > allocator_;
		std::unique_ptr< detail::watchdog_state > watchdog_;
		std::unique_ptr< std::map< std::string, host_function, std::less<> > > host_functions_;
		// Held behind a unique_ptr for the same reason the watchdog is: the address
		// goes into the Lua registry, and moving the host must not move it.
		std::unique_ptr< module_loader_function > module_loader_;
	};

}
