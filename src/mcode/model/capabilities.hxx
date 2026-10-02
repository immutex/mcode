#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "mcode/model/types.hxx"
#include "mcode/support/config.hxx"

namespace mcode::model {

	// nullopt for an unknown id; the caller fails closed, since an unpriced run reports $0.00.
	[[nodiscard]] auto lookup_capabilities( std::string_view model_id )
		-> std::optional< capabilities >;

	// the user's own `[models."<id>"]` entries merge over the compiled-in table and win.
	[[nodiscard]] auto resolve_capabilities( std::string_view model_id,
		const mcode::config::merged_config* from_config ) -> std::optional< capabilities >;

	// the id is quoted because it carries `/` and `.`, which a bare TOML key cannot hold.
	[[nodiscard]] auto capability_config_section( std::string_view model_id ) -> std::string;

}
