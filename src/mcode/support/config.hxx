#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::config {

	// lowest precedence first; project scope may only add restrictions, never widen.
	enum class scope {
		managed,
		user,
		project,
		session,
	};

	[[nodiscard]] constexpr auto to_string( const scope value ) noexcept -> std::string_view {
		switch ( value ) {
			case scope::managed: return "managed";
			case scope::user: return "user";
			case scope::project: return "project";
			case scope::session: return "session";
		}

		return "user";
	}

	// everything else in a project file is rejected at load rather than ignored.
	inline constexpr auto PROJECT_WRITABLE_PREFIXES = std::array{
		std::string_view{ "permissions.deny" },
		std::string_view{ "permissions.ask" },
	};

	// A key outside these sections is a typo, and storing it silently looks like it applied.
	inline constexpr auto CONFIG_SECTIONS = std::array{
		std::string_view{ "model" },
		std::string_view{ "agent" },
		std::string_view{ "context" },
		std::string_view{ "sandbox" },
		std::string_view{ "ui" },
		std::string_view{ "extensions" },
		std::string_view{ "permissions" },
		std::string_view{ "mcp" },
		std::string_view{ "models" },
	};

	struct layer {
		scope level = scope::user;
		std::filesystem::path origin;
		toml::table values;
	};

	class merged_config {
	public:
		// A later layer overrides a scalar; arrays merge with later entries appended.
		[[nodiscard]] static auto merge( std::vector< layer > layers ) -> result< merged_config >;

		[[nodiscard]] auto get_string( std::string_view key ) const -> std::optional< std::string >;
		[[nodiscard]] auto get_int( std::string_view key ) const -> std::optional< std::int64_t >;
		[[nodiscard]] auto get_double( std::string_view key ) const -> std::optional< double >;
		[[nodiscard]] auto get_bool( std::string_view key ) const -> std::optional< bool >;

		// absent is empty; a wrong type is an error, never an empty list
		[[nodiscard]] auto get_string_array( std::string_view key ) const
			-> result< std::vector< std::string > >;

		// which layer supplied a key, for `mcode config show`.
		[[nodiscard]] auto source_of( std::string_view key ) const -> std::optional< scope >;

		[[nodiscard]] auto keys( ) const noexcept
			-> const std::map< std::string, toml::value, std::less<> >& {
			return values_;
		}

		[[nodiscard]] auto loaded_scopes( ) const noexcept -> const std::vector< scope >& {
			return loaded_;
		}

	private:
		std::map< std::string, toml::value, std::less<> > values_;
		std::map< std::string, scope, std::less<> > origins_;
		std::vector< scope > loaded_;
	};

	[[nodiscard]] auto default_layer_paths( )
		-> std::vector< std::pair< scope, std::filesystem::path > >;

	// The message for a run that has no usable config: it names the file to edit
	// and the command that writes it. Measured on a clean machine, mcode's entire
	// output was `mcode: no provider configured; set [model] provider in
	// config.toml`, exit 2, no terminal -- which names neither the file's location
	// nor `mcode setup`. A reader with no config has no way to act on it.
	[[nodiscard]] auto unconfigured_message( const std::string_view missing_key ) -> std::string;

	// A missing file is skipped; an unreadable or malformed one is an error.
	[[nodiscard]] auto load( const std::filesystem::path& project_root = { } )
		-> result< merged_config >;

	[[nodiscard]] auto load_layer( scope level, const std::filesystem::path& path )
		-> result< layer >;

}
