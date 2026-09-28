#pragma once

#include <yyjson.h>

#include <cstdlib>
#include <memory>

namespace mcode::json::detail {

	struct free_deleter {
		auto operator( )( char* pointer ) const noexcept -> void { std::free( pointer ); }
	};

	using owned_cstr = std::unique_ptr< char, free_deleter >;

	auto free_mut_doc( yyjson_mut_doc* doc ) noexcept -> void;

	// Pretty or compact. Shared so the reader's re-emit and the writer's dump
	// cannot disagree about whitespace, which would make two equal documents
	// serialize differently.
	[[nodiscard]] auto write_flags( bool pretty ) -> yyjson_write_flag;

}
