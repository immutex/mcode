#include "mcode/mcp/constants.hxx"

namespace mcode::mcp {

	auto wrap_untrusted( const std::string_view text ) -> std::string {
		auto out = std::string{ };
		out.reserve( text.size( ) + UNTRUSTED_BEGIN.size( ) + UNTRUSTED_END.size( ) + 2 );

		out.append( UNTRUSTED_BEGIN );
		out.append( text );
		out.append( UNTRUSTED_END );

		return out;
	}

}
