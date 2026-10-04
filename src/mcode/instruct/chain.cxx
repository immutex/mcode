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

		// text::truncate appends a 3-byte ellipsis, so the slack must come off the cap first.
		inline constexpr std::uintmax_t TRUNCATION_ELLIPSIS_BYTES = 3;

		// The pointer left behind, so the model can `read` the rest rather than lose it.
		auto truncation_pointer( const std::filesystem::path& file ) -> std::string {
			return "[truncated: read " + file.string( ) + " for the rest]\n";
		}

		// A budget cut names itself, so the model knows the chain was elided, not shortened.
		auto budget_truncation_pointer( const std::filesystem::path& file ) -> std::string {
			return "[truncated: over the instruction-chain budget; read " + file.string( )
				+ " for the rest]\n";
		}

		// The chain's size as `assemble_chain` joins it: each entry plus the newline it adds.
		auto joined_bytes( const instruction_chain& chain ) -> std::uintmax_t {
			auto total = std::uintmax_t{ 0 };

			for ( const auto& entry : chain.entries ) {
				total += static_cast< std::uintmax_t >( entry.text.size( ) );
				total += entry.text.ends_with( '\n' ) ? std::uintmax_t{ 0 } : std::uintmax_t{ 1 };
			}

			return total;
		}

		// Cuts the broadest entries until the chain fits, leaving the closest one whole: an
		// over-budget chain loses its outermost context, never the rules for the work at hand.
		// Returns whether it cut anything, so the caller can say so in the warnings.
		auto enforce_chain_cap( instruction_chain& chain, const std::uintmax_t cap,
			const bool budget_cut ) -> bool {
			if ( chain.entries.size( ) < 2 ) {
				return false;
			}

			auto cut = false;

			for ( auto index = std::size_t{ 0 }; index + 1 < chain.entries.size( ); ++index ) {
				const auto total = joined_bytes( chain );

				if ( total <= cap ) {
					return cut;
				}

				auto& entry = chain.entries[ index ];

				// The file cap already cut this one as far as it can; the budget pass is the
				// one that must still cut it.
				if ( !budget_cut && entry.truncated ) {
					continue;
				}

				// An earlier pass may have left a pointer; replace it rather than stack two.
				for ( const auto& previous : { truncation_pointer( entry.file ),
					budget_truncation_pointer( entry.file ) } ) {
					if ( entry.text.ends_with( previous ) ) {
						entry.text.resize( entry.text.size( ) - previous.size( ) );
					}
				}

				const auto pointer = budget_cut ? budget_truncation_pointer( entry.file )
												: truncation_pointer( entry.file );
				const auto excess = total - cap + static_cast< std::uintmax_t >( pointer.size( ) )
					+ TRUNCATION_ELLIPSIS_BYTES;
				const auto keep = static_cast< std::uintmax_t >( entry.text.size( ) ) > excess
					? entry.text.size( ) - static_cast< std::size_t >( excess )
					: std::size_t{ 0 };

				entry.text = text::truncate( entry.text, keep );
				entry.text += pointer;
				entry.truncated = true;
				cut = true;
			}

			return cut;
		}

		// The one join rule, so every cut is measured exactly as the chain will be sent.
		auto join_chain( instruction_chain& chain ) -> void {
			chain.text.clear( );

			for ( const auto& entry : chain.entries ) {
				chain.text += entry.text;

				if ( !entry.text.ends_with( '\n' ) ) {
					chain.text += '\n';
				}
			}
		}

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

		// First match per directory wins: AGENTS.md, else CLAUDE.md, else GEMINI.md.
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

		// Collected closest first, reversed before returning: broadest first, closest last.
		auto walk_ancestors( const std::filesystem::path& start ) ->
			std::vector< std::filesystem::path > {
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

		// Two caps, broadest entry first: the byte cap bounds a pathological file, the token
		// cap is the real session budget. The closest entry is never cut, so a lone
		// over-budget AGENTS.md is warned about and sent whole - only outer files are elided.
		const auto byte_cut = enforce_chain_cap( chain, CHAIN_BYTE_CAP, false );
		const auto budget_cut = enforce_chain_cap( chain,
			static_cast< std::uintmax_t >( INSTRUCTION_CHAIN_TOKEN_BUDGET ) *
				CHARS_PER_TOKEN_ESTIMATE,
			true );

		join_chain( chain );

		chain.estimated_tokens = static_cast< std::int64_t >(
			chain.text.size( ) / CHARS_PER_TOKEN_ESTIMATE );

		// The closest entry can still be over budget on its own; then nothing was cut and the
		// lint warning is the only signal. Otherwise a cut is named, so the user knows the
		// outermost instructions were elided rather than shortened.
		if ( chain.estimated_tokens > INSTRUCTION_CHAIN_TOKEN_BUDGET ) {
			chain.warnings.push_back( "instruction chain is about " +
				std::to_string( chain.estimated_tokens ) + " tokens, over the " +
				std::to_string( INSTRUCTION_CHAIN_TOKEN_BUDGET ) + " budget; prune what "
				"the model can infer" );
		} else if ( byte_cut || budget_cut ) {
			chain.warnings.push_back( "instruction chain was over the " +
				std::to_string( INSTRUCTION_CHAIN_TOKEN_BUDGET ) + "-token budget; its "
				"broadest entries were truncated - prune what the model can infer" );
		}

		return chain;
	}

}
