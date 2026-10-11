#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::cli::detail {

	// One place, so the header looked for and the header written cannot drift apart.
	inline constexpr std::string_view SECTION_NAME = "model";
	inline constexpr std::string_view CONFIG_FILE_NAME = "config.toml";

	// Mirrors the shipped provider descriptors, so a config written here is one
	// the extension can talk to.
	struct provider_choice {
		std::string_view label;
		std::string_view descriptor;
		std::string_view base_url;
		std::string_view api_key_env;
		std::string_view note;
	};

	inline constexpr auto PROVIDERS = std::array{
		provider_choice{ "[OI]", "openai-chat-completions",
			"https://api.openai.com/v1/chat/completions", "OPENAI_API_KEY",
			"platform.openai.com" },
		provider_choice{ "Anthropic", "anthropic-messages",
			"https://api.anthropic.com/v1/messages", "ANTHROPIC_API_KEY",
			"console.anthropic.com" },
		provider_choice{ "[OI]-compatible endpoint", "openai-chat-completions",
			"", "MCODE_API_KEY", "any /v1/chat/completions gateway" },
	};

	// Only models the compiled-in table prices; others need a `[models."id"]` block.
	[[nodiscard]] auto suggested_models( std::size_t provider_index )
		-> std::vector< std::string >;

	[[nodiscard]] auto escape_toml( std::string_view text ) -> std::string;

	struct model_settings {
		std::string provider;
		std::string model;
		std::string base_url;
		std::string api_key_env;
	};

	[[nodiscard]] auto render_section( const model_settings& settings ) -> std::string;

	// Preserves everything outside the section: a parse-and-reserialise round trip
	// would drop the comments that carry the `[models."..."]` pricing rationale.
	[[nodiscard]] auto splice_section( std::string_view existing,
		const std::string& section ) -> std::string;

	// An absent file is an empty document, not a failure; an unreadable one is.
	[[nodiscard]] auto read_text_file( const std::filesystem::path& path )
		-> std::optional< std::string >;

	[[nodiscard]] auto write_text_file( const std::filesystem::path& path,
		std::string_view text ) -> bool;

	[[nodiscard]] auto config_path( ) -> std::filesystem::path;

	[[nodiscard]] auto self_path( ) -> std::filesystem::path;

}
