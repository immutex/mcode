#include "mcode/support/json_internal.hxx"

namespace mcode::json::detail {

	auto free_mut_doc( yyjson_mut_doc* doc ) noexcept -> void {
		if ( doc != nullptr ) {
			yyjson_mut_doc_free( doc );
		}
	}

	auto write_flags( const bool pretty ) -> yyjson_write_flag {
		return pretty ? YYJSON_WRITE_PRETTY : YYJSON_WRITE_NOFLAG;
	}

}
