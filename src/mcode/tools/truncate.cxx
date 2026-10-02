#include "mcode/tools/truncate.hxx"

#include <cstdint>
#include <string>

#include "mcode/fs/workspace.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools {

	namespace {

		// the run-id directory separates runs; this only separates calls within one
		auto spill_sequence = std::atomic< std::uint64_t >{ 0 };

		auto artifact_path( std::string_view run_id, std::string_view tool_name ) -> std::string {
			const auto sequence = spill_sequence.fetch_add( 1, std::memory_order_relaxed );

			auto path = std::string{ ".mcode/artifacts/" };
			path.append( run_id );
			path += '/';
			path.append( tool_name );
			path += "-" + std::to_string( sequence ) + ".txt";

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

		// a failed spill must not look like a delivered result: state that the full text is gone
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
