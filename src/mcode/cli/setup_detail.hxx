#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

// Shared by the `setup` wizard's translation units: the provider catalogue and
// the config-file editor. Neither is part of the CLI's interface, and both are
// used by the flow in `setup.cxx`, so they live in one named place rather than
// in an anonymous namespace only one file can see.
namespace mcode::cli::detail {

	// The `[model]` section this writes, as its bare name. One place, so the
	// header it looks for and the header it writes cannot drift apart.
	inline constexpr std::string_view SECTION_NAME = "model";
	inline constexpr std::string_view CONFIG_FILE_NAME = "config.toml";

	// Endpoints and key names mirror the shipped provider descriptors, so a
	// config written here is one the extension already knows how to talk to.
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

	// Only the models the compiled-in table prices. A model outside it needs a
	// `[models."id"]` block, which the wizard names rather than silently writing
	// a config that would refuse to run.
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

	// Replaces the `[model]` section in `existing`, or appends it. `section` is
	// the already-rendered replacement. Everything outside the section is
	// preserved: the file is edited as text because a parse-and-reserialise
	// round trip would drop the comments that carry the `[models."..."]`
	// pricing rationale, and no TOML writer exists here.
	[[nodiscard]] auto splice_section( std::string_view existing,
		const std::string& section ) -> std::string;

	// An absent file is an empty document, not a failure; an unreadable one is.
	[[nodiscard]] auto read_text_file( const std::filesystem::path& path )
		-> std::optional< std::string >;

	[[nodiscard]] auto write_text_file( const std::filesystem::path& path,
		std::string_view text ) -> bool;

	[[nodiscard]] auto config_path( ) -> std::filesystem::path;

	// The running executable's own path, for the verification child.
	[[nodiscard]] auto self_path( ) -> std::filesystem::path;

}
