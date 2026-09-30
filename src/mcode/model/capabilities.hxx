#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "mcode/model/types.hxx"
#include "mcode/support/config.hxx"

namespace mcode::model {

	// Looks a model id up in the compiled-in capabilities table. `nullopt` for
	// an unknown id, and the caller fails closed: running an unknown model
	// would report $0.00 spent, which silently turns budget enforcement into a
	// no-op.
	[[nodiscard]] auto lookup_capabilities( std::string_view model_id )
		-> std::optional< capabilities >;

	// The compiled-in table, with the user's own entries merged over it.
	//
	// The table exists so an unpriced run fails closed rather than reporting
	// $0.00 spent, which would silently turn budget enforcement into a no-op.
	// That reasoning assumes the table can know every model, and it cannot: a
	// gateway serving hundreds of ids is normal, and refusing all but the few
	// the binary was built with makes the harness unusable against one. So a
	// user prices their own models in `[models."<id>"]`, and the fail-closed
	// default is unchanged for every model neither source knows.
	//
	// A model named in both places takes the config's values, so a stale
	// compiled-in price can be corrected without a rebuild.
	[[nodiscard]] auto resolve_capabilities( std::string_view model_id,
		const mcode::config::merged_config* from_config ) -> std::optional< capabilities >;

	// The section a user writes to price `model_id`: `[models."<id>"]`.
	//
	// Exposed so a refusal can name the exact line to write. The id is quoted
	// because it carries `/` and `.`, which a bare TOML key cannot hold; a
	// quoted segment is taken verbatim, so the entry flattens to exactly
	// `models.<id>` and matches the id as written.
	[[nodiscard]] auto capability_config_section( std::string_view model_id ) -> std::string;

}
