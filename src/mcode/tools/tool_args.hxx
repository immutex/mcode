#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::tools {

	// The arguments a handler receives after the model's payload has been repaired and checked
	// against the schema the model saw. `failure` is the already-rendered tool-error object.
	struct prepared_arguments {
		std::string json;
		std::string failure;

		[[nodiscard]] auto ok( ) const noexcept -> bool { return failure.empty( ); }
	};

	// Repairs a model-produced payload and validates it against a tool's schema: cosmetic
	// breaks only, never inventing content. An empty schema means the tool is unconstrained.
	[[nodiscard]] auto prepare_arguments( std::string_view schema_json,
		std::string_view args_json ) -> prepared_arguments;

	class tool_args {
	public:
		// strict: a payload that is not already JSON is an error; model output goes through
		// prepare_arguments, which repairs first
		[[nodiscard]] static auto parse( std::string_view args_json )
			-> result< tool_args >;

		[[nodiscard]] auto string_field( std::string_view key ) const
			-> std::optional< std::string >;
		[[nodiscard]] auto int_field( std::string_view key ) const -> std::optional< std::int64_t >;
		[[nodiscard]] auto bool_field( std::string_view key ) const -> std::optional< bool >;
		[[nodiscard]] auto string_array_field( std::string_view key ) const
			-> std::optional< std::vector< std::string > >;

	private:
		json::document document_;
	};

}
