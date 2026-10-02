#include "mcode/skills/index.hxx"

#include <algorithm>

#include "mcode/support/text.hxx"

namespace mcode::skills {

	namespace {

		inline constexpr std::size_t ROUTING_DESCRIPTION_LENGTH = 200;

		auto visible( const skill_entry& entry ) noexcept -> bool {
			return !entry.disable_model_invocation;
		}

	} // namespace

	auto sanitize_description( const std::string_view description ) -> std::string {
		auto out = std::string{ };
		out.reserve( description.size( ) );

		for ( const auto character : description ) {
			const auto value = static_cast< unsigned char >( character );

			if ( value < 0x20 && character != '\t' ) {
				continue;
			}

			if ( value == 0x7F ) {
				continue;
			}

			switch ( character ) {
				case '<':
					out += "&lt;";

					continue;
				case '>':
					out += "&gt;";

					continue;
				case '&':
					out += "&amp;";

					continue;
				default:
					break;
			}

			out += character;
		}

		return out;
	}

	auto hidden_from_model( const skill_entry& entry ) noexcept -> bool {
		return !visible( entry );
	}

	auto index_lines( const std::vector< skill_entry >& entries ) -> std::vector< index_line > {
		auto lines = std::vector< index_line >{ };

		for ( const auto& entry : entries ) {
			lines.push_back( { .name = entry.name,
				.description = sanitize_description( entry.description ),
				.hidden = hidden_from_model( entry ) } );
		}

		std::sort( lines.begin( ), lines.end( ),
			[]( const index_line& left, const index_line& right ) {
				return left.name < right.name;
			} );

		return lines;
	}

	auto render_index( const std::vector< skill_entry >& entries ) -> std::string {
		auto out = std::string{ };

		// Through `index_lines`, so the prompt and the CLI share one ordering and sanitization.
		for ( const auto& line : index_lines( entries ) ) {
			if ( line.hidden ) {
				continue;
			}

			out += line.name;
			out += " — ";
			out += text::truncate( line.description, ROUTING_DESCRIPTION_LENGTH );
			out += '\n';
		}

		return out;
	}

}
