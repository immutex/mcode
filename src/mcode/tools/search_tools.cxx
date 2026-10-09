#include "mcode/tools/search_tools.hxx"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/glob.hxx"
#include "mcode/support/text.hxx"

namespace mcode::tools {

	namespace {

		inline constexpr std::size_t DEFAULT_GREP_MATCHES = 50;
		inline constexpr std::size_t MAX_GREP_MATCHES = 200;

		// std::regex backtracks catastrophically, so the cap is per file, enforced mid-scan
		inline constexpr std::uintmax_t GREP_FILE_BYTES = 1u * 1024u * 1024u;

		// lines longer than this are skipped: matching inside them is where backtracking lives
		inline constexpr std::size_t GREP_LINE_BYTES = 8u * 1024u;

		// The line cap does not bound backtracking: `(a+)+$` against a full line
		// is exponential in its length. This is the wall-clock bound on one call.
		inline constexpr auto GREP_SCAN_BUDGET = std::chrono::milliseconds{ 2'000 };

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

		inline constexpr std::size_t GREP_CONTEXT_CHARS = 120;

		// candidates per possible match, plus a floor, so a narrow pattern still sees the tree
		inline constexpr std::size_t GLOB_BUDGET_PER_RESULT = 4;
		inline constexpr std::size_t GLOB_BUDGET_BASE = 256;

		// The most paths a single glob may ask the walk to enumerate. It is a
		// bound on the WORK, not on the result: `max_results` multiplies into the
		// walk budget, so an unbounded value overflows it to a small number and
		// the caller asking for more results gets fewer.
		inline constexpr std::int64_t MAX_GLOB_LIMIT = 100'000;

		// the workspace walk is ignore-blind; these would burn the whole result budget on artifacts
		[[nodiscard]] auto always_skipped( const std::string_view name ) noexcept -> bool {
			return name == ".git" || name == ".mcode" ||
				name == "build" || name == "node_modules" ||
				name == ".vs" || name == ".vscode" || name == "out";
		}

		// root .gitignore only: one pattern per line, # comments, trailing / = dir, * wildcards
		class ignore_rules {
		public:
			auto load( const std::filesystem::path& root ) -> void {
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
			[[nodiscard]] auto matches( const std::string_view relative ) const noexcept -> bool {
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
			[[nodiscard]] static auto glob_match( const std::string_view pattern,
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

		private:
			struct rule {
				std::string pattern;
				bool directory_only = false;
			};

			std::vector< rule > rules_;
		};

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

	auto handle_glob( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto pattern = args.string_field( "pattern" );

		if ( !pattern || pattern->empty( ) ) {
			return error_result( "missing required argument: pattern",
				"pass a glob pattern relative to the workspace root, e.g. src/**/*.cxx", false );
		}

		auto max_results = std::size_t{ DEFAULT_GLOB_LIMIT };

		if ( const auto requested = args.int_field( "max_results" ) ) {
			if ( *requested < 1 ) {
				return error_result( "max_results must be positive",
					"pass max_results >= 1; the default budget is " +
						std::to_string( DEFAULT_GLOB_LIMIT ) + " paths", false );
			}

			// Clamped before the cast and the multiply below. The value is the
			// model's own, and an unbounded one overflows `max_results *
			// GLOB_BUDGET_PER_RESULT` to a small number -- which would shrink the
			// walk budget rather than grow it, and the caller asked for MORE
			// results. The ceiling is what the walk can actually afford to
			// enumerate, not a limit on what may be returned.
			const auto bounded = std::min< std::int64_t >( *requested, MAX_GLOB_LIMIT );
			max_results = static_cast< std::size_t >( bounded );
		}

		auto& space = *context.space;

		if ( context.permissions == nullptr ) {
			return error_result( "the permission engine is not attached",
				"construct the engine and assign tool_context::permissions "
				"before registering tools",
				false );
		}

		auto rules = ignore_rules{ };
		rules.load( space.root( ) );

		// the workspace walk cannot be reused: it applies the pattern per segment, ignore-blind
		const auto budget = max_results * GLOB_BUDGET_PER_RESULT + GLOB_BUDGET_BASE;
		auto candidates = collect_paths( space, rules, budget );

		auto filtered = std::size_t{ 0 };
		auto matches = std::vector< std::string >{ };

		for ( const auto& relative : candidates ) {
			if ( glob_matches_pattern( relative, *pattern ) ) {
				if ( matches.size( ) < max_results ) {
					matches.push_back( relative );
				} else {
					++filtered;
				}
			}
		}

		auto out = std::string{ "{\"ok\":true,\"matches\":[" };
		auto first = true;

		for ( const auto& match : matches ) {
			if ( !first ) {
				out += ',';
			}

			first = false;
			out += '"';
			json::append_escaped( out, match );
			out += '"';
		}

		out += "]";
		out += ",\"count\":" + std::to_string( matches.size( ) );

		if ( filtered > 0 ) {
			out += ",\"filtered\":" + std::to_string( filtered );
			out += ",\"hint\":\"raise max_results (default 1000) or narrow the pattern\"";
		}

		out += "}";

		return truncate_result( out, context.run_id, space, "glob" );
	}

	auto handle_grep( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto pattern = args.string_field( "pattern" );

		if ( !pattern || pattern->empty( ) ) {
			return error_result( "missing required argument: pattern",
				"pass an ECMAScript regex, e.g. void handle_.*\\( ", false );
		}

		auto max_matches = std::size_t{ DEFAULT_GREP_MATCHES };

		if ( const auto requested = args.int_field( "max_matches" ) ) {
			if ( *requested < 1 ) {
				return error_result( "max_matches must be positive",
					"pass max_matches >= 1; the default cap is 50", false );
			}

			max_matches = static_cast< std::size_t >( std::min< std::int64_t >( *requested,
				MAX_GREP_MATCHES ) );
		}

		// `std::regex` cannot be interrupted mid-match, so a wall-clock budget
		// between lines does not help: the measured cost was 13 s inside ONE
		// `regex_search` on a 26-character line, and the line cap does not bound
		// that. The only bound available at this seam is to refuse the pattern
		// shapes whose cost is exponential -- a quantifier applied to a group
		// that itself contains a quantifier, which is the classic `(a+)+$`.
		// Refusing is honest: a silent 13-second stall reads as a hang.
		if ( has_nested_quantifier( *pattern ) ) {
			return error_result( "regex rejected: nested quantifiers backtrack "
				"exponentially and cannot be time-bounded here",
				"rewrite without a quantifier inside a quantified group -- `(a+)+` "
				"becomes `a+` when the intent is one-or-more",
				false );
		}

		auto compiled = std::optional< std::regex >{ };

		try {
			compiled = std::regex{ std::string{ *pattern }, std::regex::ECMAScript };
		} catch ( const std::regex_error& error ) {
			return error_result( "invalid regex: " + std::string{ error.what( ) },
				"fix the pattern; the regex dialect is ECMAScript, and unbalanced groups "
				"or an unknown escape are the usual causes",
				false );
		}

		auto& space = *context.space;

		if ( context.permissions == nullptr ) {
			return error_result( "the permission engine is not attached",
				"construct the engine and assign tool_context::permissions "
				"before registering tools",
				false );
		}

		auto rules = ignore_rules{ };
		rules.load( space.root( ) );

		auto scope = std::string{ "." };

		if ( const auto requested = args.string_field( "path" );
			requested && !requested->empty( ) ) {
			auto resolved = space.resolve( *requested );

			if ( !resolved ) {
				return error_result( "cannot search " + *requested,
					"grep searches inside the workspace root; "
					"pass a path that stays within it", false );
			}

			auto error_code = std::error_code{ };

			if ( !std::filesystem::exists( platform::to_extended_path( *resolved ), error_code ) ||
				error_code ) {
				return error_result( "path not found: " + *requested,
					"check the path; grep searches a file or directory "
					"inside the workspace root", false );
			}

			scope = space.display_path( *resolved );
		}

		auto name_filter = std::optional< std::string >{ };

		if ( const auto requested = args.string_field( "glob" );
			requested && !requested->empty( ) ) {
			name_filter = *requested;
		}

		// a partial scan must never read as proof of absence
		auto candidates = collect_paths( space, rules, DEFAULT_GLOB_LIMIT + 1 );
		auto partial_coverage = candidates.size( ) > DEFAULT_GLOB_LIMIT;

		if ( partial_coverage ) {
			candidates.resize( DEFAULT_GLOB_LIMIT );
		}

		if ( scope != "." ) {
			auto scoped = std::vector< std::string >{ };

			for ( const auto& candidate : candidates ) {
				if ( candidate == scope || candidate.starts_with( scope + "/" ) ) {
					scoped.push_back( candidate );
				}
			}

			candidates = std::move( scoped );
		}

		auto out = std::string{ "{\"ok\":true,\"matches\":[" };
		auto count = std::size_t{ 0 };
		auto files_scanned = std::size_t{ 0 };
		auto files_skipped = std::size_t{ 0 };
		auto first = true;
		auto reached_cap = false;
		auto timed_out = false;

		// One budget for the whole call, not per file: a per-file reset lets a
		// scan over many files each burn the full budget.
		const auto deadline = std::chrono::steady_clock::now( ) + GREP_SCAN_BUDGET;

		for ( const auto& relative : candidates ) {
			if ( count >= max_matches ) {
				reached_cap = true;

				break;
			}

			if ( std::chrono::steady_clock::now( ) >= deadline ) {
				timed_out = true;

				break;
			}

			if ( name_filter && !support::wildcard_match( *name_filter,
				relative.substr( relative.rfind( '/' ) + 1 ) ) &&
				!support::wildcard_match( *name_filter, relative ) ) {
				continue;
			}

			auto error_code = std::error_code{ };
			const auto size = std::filesystem::file_size(
				platform::to_extended_path( space.root( ) / std::filesystem::path{ relative } ),
				error_code );

			if ( error_code || size > GREP_FILE_BYTES ) {
				++files_skipped;

				continue;
			}

			auto input = std::ifstream{
				platform::to_extended_path( space.root( ) / std::filesystem::path{ relative } ),
				std::ios::binary };

			if ( !input ) {
				++files_skipped;

				continue;
			}

			auto content = std::string{ };
			content.resize( static_cast< std::size_t >( size ) );
			input.read( content.data( ), static_cast< std::streamsize >( content.size( ) ) );
			content.resize( static_cast< std::size_t >( input.gcount( ) ) );

			if ( looks_binary( content ) ) {
				++files_skipped;

				continue;
			}

			++files_scanned;

			const auto safe = text::sanitize_utf8( content );
			auto line_number = std::size_t{ 0 };
			auto start = std::size_t{ 0 };

			while ( start < safe.size( ) && count < max_matches ) {
				// `std::regex` backtracks, and the line cap does not bound that:
				// a pattern like `(a+)+$` against 8 KiB of `a` is exponential in
				// the line length, measured at seconds for a 26-character line.
				// The budget is wall clock rather than a step count because the
				// matcher gives no way to interrupt it mid-call.
				if ( std::chrono::steady_clock::now( ) >= deadline ) {
					timed_out = true;

					break;
				}

				const auto newline = safe.find( '\n', start );
				const auto end = ( newline == std::string::npos ) ? safe.size( ) : newline;
				const auto line = safe.substr( start, end - start );
				++line_number;

				if ( line.size( ) <= GREP_LINE_BYTES &&
					std::regex_search( line, *compiled ) ) {
					auto excerpt = std::string{ line.substr( 0, GREP_CONTEXT_CHARS ) };

					if ( !first ) {
						out += ',';
					}

					first = false;
					out += "{\"path\":\"";
					json::append_escaped( out, relative );
					out += "\",\"line\":" + std::to_string( line_number );
					out += ",\"text\":\"";
					json::append_escaped( out, excerpt );
					out += "\"}";

					++count;
				}

				if ( newline == std::string::npos ) {
					break;
				}

				start = newline + 1;
			}
		}

		out += "],\"count\":" + std::to_string( count );

		if ( reached_cap ) {
			out += ",\"hint\":\"cap of " + std::to_string( max_matches ) +
				" matches reached; narrow the pattern, set path to a subdirectory, "
				"or raise max_matches (max " +
				std::to_string( MAX_GREP_MATCHES ) + ") to see more\"";
		}

		// The scan stopped early, so an empty or short result is not evidence of
		// absence -- which is what the pattern's own backtracking cost caused.
		if ( timed_out ) {
			out += ",\"hint\":\"the scan hit its " +
				std::to_string( GREP_SCAN_BUDGET.count( ) ) +
				"ms budget and is INCOMPLETE; a backtracking pattern is the usual "
				"cause, so simplify it or narrow path\"";
		}

		out += ",\"files_scanned\":" + std::to_string( files_scanned );

		if ( files_skipped > 0 ) {
			out += ",\"files_skipped\":" + std::to_string( files_skipped );
		}

		if ( partial_coverage ) {
			out += ",\"partial_coverage\":true,\"hint\":\"the workspace has more than " +
				std::to_string( DEFAULT_GLOB_LIMIT ) +
				" candidate files and the scan stopped there; a zero count above is "
				"NOT proof of absence -- narrow with path or glob and search again\"";
		}

		out += "}";

		return truncate_result( out, context.run_id, space, "grep" );
	}

}
