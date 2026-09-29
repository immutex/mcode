#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "mcode/support/json.hxx"

namespace mcode::tools {

	// Parses a tool-call argument object once and reads typed fields from it.
	// Every tool goes through this, so a malformed argument gets one error shape
	// and a missing field gets one message convention.
	class tool_args {
	public:
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
