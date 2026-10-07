#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::tools {

	// The arguments a handler is about to receive, after the model's payload has been repaired
	// and checked against the schema the model was shown. `failure` is set instead when the call
	// cannot proceed, and is already the rendered tool-error object to hand back.
	struct prepared_arguments {
		std::string json;
		std::string failure;

		[[nodiscard]] auto ok( ) const noexcept -> bool { return failure.empty( ); }
	};

	// Repairs a model-produced argument payload and validates it against a tool's schema.
	// Repairs only cosmetic breaks - a markdown fence, surrounding prose, a trailing comma,
	// single quotes, Python literals, a tail cut at a value boundary - and never invents
	// content. An empty schema means the tool is unconstrained and only repair applies.
	[[nodiscard]] auto prepare_arguments( std::string_view schema_json,
		std::string_view args_json ) -> prepared_arguments;

	class tool_args {
	public:
		// Strict: a payload that is not already JSON is an error. Callers on a model's output
		// path want prepare_arguments, which repairs first.
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
