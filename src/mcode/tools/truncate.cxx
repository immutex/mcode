#include "mcode/tools/truncate.hxx"

#include <string>

#include "mcode/fs/workspace.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools {

	namespace {

		auto artifact_path( std::string_view run_id, std::string_view tool_name ) -> std::string {
			auto path = std::string{ ".mcode/artifacts/" };
			path.append( run_id );
			path += '/';
			path.append( tool_name );
			path += ".txt";

			return path;
		}

		auto cut_at_codepoint( std::string_view text, std::size_t max_bytes ) -> std::size_t {
			return text::truncate_offset( text, max_bytes );
		}

	}

	auto needs_truncation( std::string_view text ) noexcept -> bool {
		return text.size( ) > INLINE_RESULT_CHARS;
	}

	auto truncate_result( const std::string_view text, const std::string_view run_id,
		workspace& space, const std::string_view tool_name ) -> std::string {
		if ( text.size( ) <= INLINE_RESULT_CHARS ) {
			return std::string{ text };
		}

		const auto path = artifact_path( run_id, tool_name );
		auto spilled = space.write_file( path, text, write_mode::create );

		// A failed spill must not masquerade as a delivered result. Hard-cut
		// instead, with the failure stated, so the model knows the full text is
		// NOT retrievable.
		if ( !spilled ) {
			const auto keep = cut_at_codepoint( text, HARD_RESULT_CHARS );
			auto out = std::string{ text.substr( 0, keep ) };
			out += "\n[truncation failed: the full output could not be spilled to " + path +
				" (" + spilled.error( ).msg + ")]";

			return out;
		}

		const auto preview_bytes = cut_at_codepoint( text, SPILL_PREVIEW_CHARS );
		auto out = std::string{ text.substr( 0, preview_bytes ) };

		if ( preview_bytes < text.size( ) ) {
			out += "\n...";
		}

		out += "\n[output truncated: showing the first " + std::to_string( preview_bytes ) +
			" of " + std::to_string( text.size( ) ) + " characters. Full output written to " +
			path + "; re-read it with the read tool on that path]";

		return out;
	}

}
