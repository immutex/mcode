#include "mcode/instruct/chain.hxx"

#include <algorithm>
#include <fstream>

#include "mcode/agent/loop.hxx"
#include "mcode/support/text.hxx"

namespace mcode::instruct {

	namespace {

		inline constexpr std::uintmax_t FILE_BYTE_CAP = 32u * 1024u;
		inline constexpr std::uintmax_t CHAIN_BYTE_CAP = 64u * 1024u;
		inline constexpr std::size_t MAX_WALK_DEPTH = 32;

		// The pointer a truncated file leaves, so the model can `read` the rest
		// instead of the harness silently dropping instructions.
		auto truncation_pointer( const std::filesystem::path& file ) -> std::string {
			return "[truncated: read " + file.string( ) + " for the rest]\n";
		}

		// text::truncate appends a three-byte ellipsis after the cut, so a cut
		// to `keep` yields `keep + 3` bytes. The slack is subtracted up front,
		// otherwise every truncation lands the chain a few bytes over the cap
		// and the loop cuts the next file for no reason.
		inline constexpr std::uintmax_t TRUNCATION_ELLIPSIS_BYTES = 3;

		auto read_head( const std::filesystem::path& file ) -> result< std::string > {
			auto stream = std::ifstream{ file, std::ios::binary };

			if ( !stream ) {
				return std::unexpected( fail( errc::io, "cannot open " + file.string( ) ) );
			}

			auto buffer = std::string( FILE_BYTE_CAP, '\0' );

			stream.read( buffer.data( ), static_cast< std::streamsize >( buffer.size( ) ) );
			buffer.resize( static_cast< std::size_t >( stream.gcount( ) ) );

			return buffer;
		}

		auto is_repo_root( const std::filesystem::path& directory ) -> bool {
			auto error = std::error_code{ };

			return std::filesystem::exists( directory / ".git", error ) && !error;
		}

		// First match per directory wins: AGENTS.md, else CLAUDE.md, else
		// GEMINI.md. Reading all three would triple the budget for the common
		// case where they are copies.
		auto instruction_file( const std::filesystem::path& directory ) -> std::filesystem::path {
			for ( const auto* name : { "AGENTS.md", "CLAUDE.md", "GEMINI.md" } ) {
				const auto candidate = directory / name;

				auto error = std::error_code{ };

				if ( std::filesystem::is_regular_file( candidate, error ) && !error ) {
					return candidate;
				}
			}

			return { };
		}

		// The walk collects closest first, so it is reversed before returning:
		// the chain is broadest first and closest last, which is what makes the
		// closest file win on contradiction.
		auto walk_ancestors( const std::filesystem::path& start ) -> std::vector< std::filesystem::path > {
			auto files = std::vector< std::filesystem::path >{ };
			auto directory = std::filesystem::absolute( start ).lexically_normal( );
			auto depth = std::size_t{ 0 };

			while ( depth < MAX_WALK_DEPTH ) {
				if ( auto found = instruction_file( directory ); !found.empty( ) ) {
					files.push_back( found );
				}

				if ( is_repo_root( directory ) || directory == directory.root_path( ) ||
					directory == directory.parent_path( ) ) {
					break;
				}

				directory = directory.parent_path( );
				++depth;
			}

			std::reverse( files.begin( ), files.end( ) );

			return files;
		}

	} // namespace

	auto assemble_chain( const chain_options& options ) -> instruction_chain {
		auto chain = instruction_chain{ };
		auto files = walk_ancestors( options.start );

		if ( !options.user_file.empty( ) && std::filesystem::exists( options.user_file ) ) {
			files.insert( files.begin( ), options.user_file );
		}

		if ( !options.org_file.empty( ) && std::filesystem::exists( options.org_file ) ) {
			files.insert( files.begin( ), options.org_file );
		}

		for ( const auto& file : files ) {
			auto content = read_head( file );

			if ( !content ) {
				continue;
			}

			auto entry = chain_entry{ };
			entry.file = file;
			entry.text = std::move( *content );
			entry.truncated = static_cast< std::uintmax_t >( entry.text.size( ) )
				>= FILE_BYTE_CAP;

			if ( entry.truncated ) {
				entry.text += truncation_pointer( file );
			}

			chain.entries.push_back( std::move( entry ) );
		}

		// The broadest file is cut first when the chain exceeds its cap: the
		// closest file is the one the user is standing in, and it wins on
		// contradiction, so it is never the one dropped.
		auto total = std::uintmax_t{ 0 };

		for ( const auto& entry : chain.entries ) {
			total += static_cast< std::uintmax_t >( entry.text.size( ) );
		}

		if ( total > CHAIN_BYTE_CAP ) {
			for ( auto index = std::size_t{ 0 };
				index < chain.entries.size( ) && total > CHAIN_BYTE_CAP; ++index ) {
				auto& entry = chain.entries[ index ];

				if ( entry.truncated ) {
					continue;
				}

				const auto pointer = truncation_pointer( entry.file );
				const auto excess = total - CHAIN_BYTE_CAP
					+ static_cast< std::uintmax_t >( pointer.size( ) )
					+ TRUNCATION_ELLIPSIS_BYTES;
				const auto keep = static_cast< std::uintmax_t >( entry.text.size( ) ) > excess
					? entry.text.size( ) - static_cast< std::size_t >( excess )
					: std::size_t{ 0 };

				entry.text = text::truncate( entry.text, keep );
				entry.text += pointer;
				entry.truncated = true;

				// The truncate helper appends an ellipsis, so the new size is not
				// exactly `keep + pointer.size( )`. Recount rather than predict.
				total = std::uintmax_t{ 0 };

				for ( const auto& counted : chain.entries ) {
					total += static_cast< std::uintmax_t >( counted.text.size( ) );
				}
			}
		}

		for ( const auto& entry : chain.entries ) {
			chain.text += entry.text;

			if ( !entry.text.ends_with( "\n" ) ) {
				chain.text += '\n';
			}
		}

		chain.estimated_tokens = static_cast< std::int64_t >(
			chain.text.size( ) / CHARS_PER_TOKEN_ESTIMATE );

		// The token budget is a lint warning, never a truncation. Dropping user
		// instructions to hit a token figure is worse than saying the chain is
		// fat.
		if ( chain.estimated_tokens > INSTRUCTION_CHAIN_TOKEN_BUDGET ) {
			chain.warnings.push_back( "instruction chain is about " +
				std::to_string( chain.estimated_tokens ) + " tokens, over the " +
				std::to_string( INSTRUCTION_CHAIN_TOKEN_BUDGET ) + " budget; prune what "
				"the model can infer" );
		}

		return chain;
	}

}
