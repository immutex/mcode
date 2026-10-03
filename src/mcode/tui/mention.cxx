#include "mcode/tui/mention.hxx"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace mcode::tui {

	namespace {

		[[nodiscard]] auto folded( const char character ) -> char {
			return static_cast< char >( std::tolower( static_cast< unsigned char >( character ) ) );
		}

		[[nodiscard]] auto starts_with_ci( const std::string_view text,
			const std::string_view prefix ) -> bool {
			if ( prefix.size( ) > text.size( ) ) {
				return false;
			}

			for ( auto index = std::size_t{ 0 }; index < prefix.size( ); ++index ) {
				if ( folded( text[ index ] ) != folded( prefix[ index ] ) ) {
					return false;
				}
			}

			return true;
		}

		[[nodiscard]] auto subsequence_match( const std::string_view query,
			const std::string_view text ) -> bool {
			auto index = std::size_t{ 0 };

			for ( const auto character : text ) {
				if ( index < query.size( ) && folded( character ) == folded( query[ index ] ) ) {
					++index;
				}
			}

			return index == query.size( );
		}

		[[nodiscard]] auto is_word_character( const char character ) noexcept -> bool {
			const auto plain = static_cast< unsigned char >( character );

			return std::isalnum( plain ) != 0 || character == '.' || character == '@';
		}

		[[nodiscard]] auto basename_of( const std::string_view path ) -> std::string_view {
			const auto slash = path.rfind( '/' );

			return slash == std::string_view::npos ? path : path.substr( slash + 1 );
		}

		// The offset of the '@' that opens the mention, or npos. Private, so
		// the query and the completion can never disagree on the span.
		[[nodiscard]] auto mention_offset( const std::string_view input ) -> std::size_t {
			const auto at = input.rfind( '@' );

			if ( at == std::string_view::npos ) {
				return std::string_view::npos;
			}

			// A mention starts a word: a preceding letter, digit, dot or '@'
			// means an address like name@host, not a mention. Punctuation such
			// as '(' or '"' still opens one.
			if ( at > 0 && is_word_character( input[ at - 1 ] ) ) {
				return std::string_view::npos;
			}

			// A space closes the mention, so only the current word is a query.
			if ( input.find( ' ', at ) != std::string_view::npos ) {
				return std::string_view::npos;
			}

			return at;
		}

		// The directory names the glob tool always skips. The shared walk is
		// ignore-blind, so artifacts are dropped by name instead.
		[[nodiscard]] auto skipped_segment( const std::string_view name ) noexcept -> bool {
			return name == ".git" || name == ".mcode" || name == "build" ||
				name == "node_modules" || name == ".vs" || name == ".vscode" || name == "out";
		}

		[[nodiscard]] auto has_skipped_segment( const std::string_view relative ) -> bool {
			auto start = std::size_t{ 0 };

			while ( start <= relative.size( ) ) {
				const auto slash = relative.find( '/', start );
				const auto end = slash == std::string_view::npos ? relative.size( ) : slash;

				if ( skipped_segment( relative.substr( start, end - start ) ) ) {
					return true;
				}

				if ( slash == std::string_view::npos ) {
					break;
				}

				start = slash + 1;
			}

			return false;
		}

		[[nodiscard]] auto regular_at( const std::filesystem::path& path ) -> bool {
			auto error_code = std::error_code{ };
			const auto regular = std::filesystem::is_regular_file( path, error_code );

			return !error_code && regular;
		}

		[[nodiscard]] auto directory_at( const std::filesystem::path& path ) -> bool {
			auto error_code = std::error_code{ };
			const auto directory = std::filesystem::is_directory( path, error_code );

			return !error_code && directory;
		}

	}

	auto mention_query( const std::string_view input ) -> std::optional< std::string_view > {
		const auto at = mention_offset( input );

		if ( at == std::string_view::npos ) {
			return std::nullopt;
		}

		return input.substr( at + 1 );
	}

	auto mention_completion( const std::string_view input, const std::string_view path )
		-> std::string {
		const auto at = mention_offset( input );

		if ( at == std::string_view::npos ) {
			return std::string{ input };
		}

		return std::string{ input.substr( 0, at ) } + "@" + std::string{ path } + " ";
	}

	auto rank_mentions( const std::vector< std::string >& paths, const std::string_view query,
		const std::size_t limit ) -> std::vector< std::string > {
		auto out = std::vector< std::string >{ };

		if ( limit == 0 ) {
			return out;
		}

		if ( query.empty( ) ) {
			for ( const auto& path : paths ) {
				if ( out.size( ) >= limit ) {
					break;
				}

				out.push_back( path );
			}

			return out;
		}

		// Three tiers, best first, so each tier keeps the caller's order. A
		// path that matches no subsequence is dropped.
		const auto collect = [ & ]( const auto& accepted ) {
			for ( const auto& path : paths ) {
				if ( out.size( ) >= limit ) {
					return;
				}

				if ( accepted( path ) && subsequence_match( query, path ) ) {
					out.push_back( path );
				}
			}
		};

		collect( [ & ]( const std::string& path ) { return starts_with_ci( path, query ); } );

		collect( [ & ]( const std::string& path ) {
			return !starts_with_ci( path, query ) &&
				starts_with_ci( basename_of( path ), query );
		} );

		collect( [ & ]( const std::string& path ) {
			return !starts_with_ci( path, query ) &&
				!starts_with_ci( basename_of( path ), query );
		} );

		return out;
	}

	auto list_workspace_files( const mcode::workspace& space, const std::size_t limit )
		-> std::vector< std::string > {
		auto out = std::vector< std::string >{ };

		// Per-root descent rather than one `**/*` from the root: the shared
		// walk is ignore-blind, so a single pattern would spend the whole
		// budget inside .git or node_modules before reaching src/.
		const auto roots = space.glob( "*", limit );

		if ( !roots ) {
			return out;
		}

		const auto collect = [ & ]( const std::string& pattern ) {
			if ( out.size( ) >= limit ) {
				return;
			}

			// The walk yields directory names as well as files, so it gets the
			// whole scan budget: a share of what is left could be spent on
			// directories before any file was reached. The rank cap, not this
			// budget, bounds the result.
			const auto found = space.glob( pattern, MENTION_SCAN_LIMIT );

			if ( !found ) {
				return;
			}

			for ( const auto& path : *found ) {
				if ( out.size( ) >= limit ) {
					return;
				}

				if ( !regular_at( path ) ) {
					continue;
				}

				const auto relative = space.display_path( path );

				if ( relative.empty( ) || has_skipped_segment( relative ) ) {
					continue;
				}

				out.push_back( relative );
			}
		};

		for ( const auto& entry : *roots ) {
			if ( out.size( ) >= limit ) {
				break;
			}

			const auto name = entry.filename( ).string( );

			if ( name.empty( ) || skipped_segment( name ) ) {
				continue;
			}

			if ( directory_at( entry ) ) {
				collect( name + "/**/*" );
			} else if ( regular_at( entry ) ) {
				out.push_back( space.display_path( entry ) );
			}
		}

		std::sort( out.begin( ), out.end( ) );
		out.erase( std::unique( out.begin( ), out.end( ) ), out.end( ) );

		return out;
	}

	auto mention_index::load( ) const -> void {
		if ( loaded_ ) {
			return;
		}

		loaded_ = true;

		if ( space_ != nullptr ) {
			files_ = list_workspace_files( *space_, MENTION_SCAN_LIMIT );
		}
	}

	auto mention_index::matches( const std::string_view query ) const
		-> std::vector< slash_command > {
		load( );

		auto out = std::vector< slash_command >{ };

		for ( const auto& path : rank_mentions( files_, query, MENTION_LIMIT ) ) {
			out.push_back( slash_command{ path, std::string{ } } );
		}

		return out;
	}

}
