#include "mcode/tools/search_walk.hxx"

#include <algorithm>
#include <fstream>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/glob.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools::search_walk {

	// std::regex backtracks catastrophically, so the cap is per file, enforced mid-scan

	// lines longer than this are skipped: matching inside them is where backtracking lives

	// The line cap does not bound backtracking: `(a+)+$` against a full line
	// is exponential in its length. This is the wall-clock bound on one call.

	// candidates per possible match, plus a floor, so a narrow pattern still sees the tree

	// The most paths a single glob may ask the walk to enumerate. It is a
	// bound on the WORK, not on the result: `max_results` multiplies into the
	// walk budget, so an unbounded value overflows it to a small number and
	// the caller asking for more results gets fewer.

	// A quantifier applied to a group that contains a quantifier: `(a+)+`,
	// `(a*)*`, `(a+){2,}`, `([ab]+)*`. Those are the shapes whose match cost
	// is exponential in the input, and `std::regex` offers no way to bound a
	// single `regex_search`, so they are refused rather than run.
	[[nodiscard]] auto has_nested_quantifier( const std::string_view pattern ) -> bool {
		auto depth = std::size_t{ 0 };
		auto quantified_inside = false;
		auto escaped = false;
		auto in_class = false;

		for ( auto index = std::size_t{ 0 }; index < pattern.size( ); ++index ) {
			const auto character = pattern[ index ];

			if ( escaped ) {
				escaped = false;

				continue;
			}

			if ( character == '\\' ) {
				escaped = true;

				continue;
			}

			// A character class is not a group: `[+*]` is literals.
			if ( character == '[' ) {
				in_class = true;

				continue;
			}

			if ( character == ']' && in_class ) {
				in_class = false;

				continue;
			}

			if ( in_class ) {
				continue;
			}

			if ( character == '(' ) {
				++depth;

				if ( depth == 1 ) {
					quantified_inside = false;
				}

				continue;
			}

			if ( character == ')' ) {
				if ( depth > 0 ) {
					--depth;
				}

				// The group just closed. If it held a quantifier, a quantifier
				// on the group itself is the exponential shape.
				if ( depth == 0 && quantified_inside ) {
					const auto next = index + 1 < pattern.size( ) ? pattern[ index + 1 ]
						: '\0';

					if ( next == '*' || next == '+' || next == '{' ) {
						return true;
					}
				}

				continue;
			}

			if ( depth > 0 && ( character == '*' || character == '+' ) ) {
				quantified_inside = true;
			}
		}

		return false;
	}

	// candidates per possible match, plus a floor, so a narrow pattern still sees the tree

	// The most paths a single glob may ask the walk to enumerate. It is a
	// bound on the WORK, not on the result: `max_results` multiplies into the
	// walk budget, so an unbounded value overflows it to a small number and
	// the caller asking for more results gets fewer.

	// the workspace walk is ignore-blind; these would burn the whole result budget on artifacts
	[[nodiscard]] auto always_skipped( const std::string_view name ) noexcept -> bool {
		return name == ".git" || name == ".mcode" ||
			name == "build" || name == "node_modules" ||
			name == ".vs" || name == ".vscode" || name == "out";
	}

	auto ignore_rules::load( const std::filesystem::path& root ) -> void {
			auto input = std::ifstream{ platform::to_extended_path( root / ".gitignore" ),
				std::ios::binary };

			if ( !input ) {
				return;
			}

			auto line = std::string{ };

			while ( std::getline( input, line ) ) {
				while ( !line.empty( ) && ( line.back( ) == '\r' || line.back( ) == ' ' ) ) {
					line.pop_back( );
				}

				if ( line.empty( ) || line.front( ) == '#' || line.front( ) == '!' ) {
					continue;
				}

				auto directory_only = false;

				if ( !line.empty( ) && line.back( ) == '/' ) {
					directory_only = true;
					line.pop_back( );
				}

				while ( line.size( ) > 1 && line.front( ) == '/' ) {
					line.erase( line.begin( ) );
				}

				if ( line.empty( ) ) {
					continue;
				}

			rules_.push_back( rule{ line, directory_only } );
		}
	}

	// `relative` uses forward slashes with no leading or trailing slash.
	auto ignore_rules::matches( const std::string_view relative ) const noexcept -> bool {
			for ( const auto& entry : rules_ ) {
				if ( entry.directory_only ) {
					if ( relative == entry.pattern ||
						relative.starts_with( entry.pattern + "/" ) ) {
						return true;
					}

					continue;
				}

				if ( relative == entry.pattern ) {
					return true;
				}

				if ( entry.pattern.find( '*' ) != std::string_view::npos &&
					glob_match( entry.pattern, relative ) ) {
					return true;
				}
			}

		return false;
	}

	// `*` stays within one segment; a pattern with no `/` matches the file name anywhere
	auto ignore_rules::glob_match( const std::string_view pattern,
		const std::string_view path ) noexcept -> bool {
			const auto slash = pattern.find( '/' );

			if ( slash == std::string_view::npos ) {
				const auto last_slash = path.rfind( '/' );
				const auto name = ( last_slash == std::string_view::npos )
					? path
					: path.substr( last_slash + 1 );

				return support::wildcard_match( pattern, name );
			}

		return support::wildcard_match( pattern, path );
	}

	[[nodiscard]] auto collect_paths( const workspace& space, const ignore_rules& rules,
		std::size_t budget ) -> std::vector< std::string > {
		auto out = std::vector< std::string >{ };
		auto directories = std::vector< std::filesystem::path >{ space.root( ) };
		auto error_code = std::error_code{ };

		while ( !directories.empty( ) && out.size( ) < budget ) {
			const auto current = directories.back( );
			directories.pop_back( );

			for ( const auto& entry :
				std::filesystem::directory_iterator{ platform::to_extended_path( current ),
					std::filesystem::directory_options::skip_permission_denied, error_code } ) {
				if ( error_code ) {
					break;
				}

				if ( out.size( ) >= budget ) {
					break;
				}

				const auto name = entry.path( ).filename( ).string( );
				const auto is_directory = entry.is_directory( error_code );

				if ( error_code ) {
					error_code.clear( );

					continue;
				}

				const auto relative = space.display_path( entry.path( ) );

				if ( always_skipped( name ) ) {
					continue;
				}

				if ( rules.matches( relative ) ) {
					continue;
				}

				if ( is_directory ) {
					directories.push_back( entry.path( ) );
				} else {
					out.push_back( relative );
				}
			}
		}

		std::sort( out.begin( ), out.end( ) );

		return out;
	}

	[[nodiscard]] auto glob_matches_pattern( const std::string_view path,
		const std::string_view pattern ) -> bool {
		return support::glob_match( pattern, path );
	}
}
