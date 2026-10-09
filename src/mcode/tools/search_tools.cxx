#include "mcode/tools/search_tools.hxx"

#include "mcode/tools/search_walk.hxx"

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
			// search_walk::GLOB_BUDGET_PER_RESULT` to a small number -- which would shrink the
			// walk budget rather than grow it, and the caller asked for MORE
			// results. The ceiling is what the walk can actually afford to
			// enumerate, not a limit on what may be returned.
			const auto bounded = std::min< std::int64_t >( *requested, search_walk::MAX_GLOB_LIMIT );
			max_results = static_cast< std::size_t >( bounded );
		}

		auto& space = *context.space;

		if ( context.permissions == nullptr ) {
			return error_result( "the permission engine is not attached",
				"construct the engine and assign tool_context::permissions "
				"before registering tools",
				false );
		}

		auto rules = search_walk::ignore_rules{ };
		rules.load( space.root( ) );

		// the workspace walk cannot be reused: it applies the pattern per segment, ignore-blind
		const auto budget = max_results * search_walk::GLOB_BUDGET_PER_RESULT + search_walk::GLOB_BUDGET_BASE;
		auto candidates = search_walk::collect_paths( space, rules, budget );

		auto filtered = std::size_t{ 0 };
		auto matches = std::vector< std::string >{ };

		for ( const auto& relative : candidates ) {
			if ( search_walk::glob_matches_pattern( relative, *pattern ) ) {
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
		if ( search_walk::has_nested_quantifier( *pattern ) ) {
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

		auto rules = search_walk::ignore_rules{ };
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
		auto candidates = search_walk::collect_paths( space, rules, DEFAULT_GLOB_LIMIT + 1 );
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
		const auto deadline = std::chrono::steady_clock::now( ) + search_walk::GREP_SCAN_BUDGET;

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

			if ( error_code || size > search_walk::GREP_FILE_BYTES ) {
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

				if ( line.size( ) <= search_walk::GREP_LINE_BYTES &&
					std::regex_search( line, *compiled ) ) {
					auto excerpt = std::string{ line.substr( 0, search_walk::GREP_CONTEXT_CHARS ) };

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
				std::to_string( search_walk::GREP_SCAN_BUDGET.count( ) ) +
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
