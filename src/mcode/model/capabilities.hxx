#pragma once

#include <optional>
#include <string_view>

#include "mcode/model/types.hxx"

namespace mcode::model {

	// Looks a model id up in the compiled-in capabilities table. `nullopt` for
	// an unknown id, and the caller fails closed: running an unknown model
	// would report $0.00 spent, which silently turns budget enforcement into a
	// no-op.
	[[nodiscard]] auto lookup_capabilities( std::string_view model_id )
		-> std::optional< capabilities >;

}
