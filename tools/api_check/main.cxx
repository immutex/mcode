// Verifies that extension code type-checks against the shipped definition file.
//
// This exists because `luau-analyze` cannot do it: definition files need
// ParseOptions::allowDeclarationSyntax and Mode::Definition, and the CLI parses
// every input as an ordinary script. Without this tool the `.d.luau` file would
// be unverified prose.
//
// Usage: mcode_api_check <mcode.d.luau> <extension.luau> [more.luau ...]

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/ConfigResolver.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Luau/ParseResult.h"
#include "Luau/StringUtils.h"

namespace {

	auto read_file( const std::filesystem::path& path ) -> std::string {
		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return { };
		}

		auto buffer = std::ostringstream{ };
		buffer << stream.rdbuf( );

		return buffer.str( );
	}

	struct disk_resolver : Luau::FileResolver {
		auto readSource( const Luau::ModuleName& name ) -> std::optional< Luau::SourceCode > override {
			if ( !std::filesystem::exists( name ) ) {
				return std::nullopt;
			}

			return Luau::SourceCode{ read_file( name ), Luau::SourceCode::Type::Module };
		}
	};

	// No .luaurc discovery: the extensions being checked are not a module tree,
	// and reading config files would make the check depend on the directory it
	// happens to run in.
	struct flat_config_resolver : Luau::ConfigResolver {
		Luau::Config config;

		explicit flat_config_resolver( const Luau::Mode mode ) {
			config.mode = mode;
		}

		auto getConfig( const Luau::ModuleName&, const Luau::TypeCheckLimits& ) const
			-> const Luau::Config& override {
			return config;
		}
	};

}

auto main( int argument_count, char** arguments ) -> int {
	if ( argument_count < 3 ) {
		std::fprintf( stderr, "usage: %s <mcode.d.luau> <extension.luau> [...]\n", arguments[ 0 ] );

		return 2;
	}

	const auto definition_path = std::filesystem::path{ arguments[ 1 ] };
	const auto definition_source = read_file( definition_path );

	if ( definition_source.empty( ) ) {
		std::fprintf( stderr, "cannot read definition file: %s\n", arguments[ 1 ] );

		return 2;
	}

	auto file_resolver = disk_resolver{ };
	auto config_resolver = flat_config_resolver{ Luau::Mode::Nonstrict };

	auto options = Luau::FrontendOptions{ };
	options.runLintChecks = true;
	options.retainFullTypeGraphs = false;

	auto frontend = Luau::Frontend{ Luau::SolverMode::New, &file_resolver, &config_resolver, options };

	Luau::registerBuiltinGlobals( frontend, frontend.globals );
	Luau::freeze( frontend.globals.globalTypes );

	const auto loaded = frontend.loadDefinitionFile(
		frontend.globals,
		frontend.globals.globalScope,
		definition_source,
		definition_path.filename( ).string( ),
		/* captureComments= */ false,
		/* typeCheckForAutocomplete= */ false );

	if ( !loaded.success ) {
		std::fprintf( stderr, "definition file failed to load:\n" );

		for ( const Luau::ParseError& error : loaded.parseResult.errors ) {
			std::fprintf( stderr, "  %s(%d,%d): %s\n", definition_path.filename( ).string( ).c_str( ),
				error.getLocation( ).begin.line + 1, error.getLocation( ).begin.column + 1,
				error.getMessage( ).c_str( ) );
		}

		return 1;
	}

	auto failures = 0;
	auto checked = 0;

	for ( auto index = 2; index < argument_count; ++index ) {
		const auto module_name = std::string{ arguments[ index ] };

		if ( read_file( module_name ).empty( ) ) {
			std::fprintf( stderr, "cannot read: %s\n", module_name.c_str( ) );
			++failures;

			continue;
		}

		frontend.queueModuleCheck( module_name );

		const auto checked_modules = frontend.checkQueuedModules( );
		auto reported = 0;

		for ( const auto& name : checked_modules ) {
			auto result = frontend.getCheckResult( name, /* accumulateNested= */ false );

			if ( !result ) {
				continue;
			}

			for ( const Luau::TypeError& error : result->errors ) {
				// A syntax error arrives as a TypeError whose data is SyntaxError;
				// upstream's own CLI unwraps it the same way.
				if ( const Luau::SyntaxError* syntax = Luau::get_if< Luau::SyntaxError >( &error.data ) ) {
					std::fprintf( stderr, "%s(%d,%d): SyntaxError: %s\n", name.c_str( ),
						error.location.begin.line + 1, error.location.begin.column + 1,
						syntax->message.c_str( ) );
				} else {
					const auto message = Luau::toString(
						error, Luau::TypeErrorToStringOptions{ frontend.fileResolver } );

					std::fprintf( stderr, "%s(%d,%d): TypeError: %s\n", name.c_str( ),
						error.location.begin.line + 1, error.location.begin.column + 1,
						message.c_str( ) );
				}

				++reported;
			}

			for ( const Luau::LintWarning& warning : result->lintResult.warnings ) {
				std::fprintf( stderr, "%s(%d,%d): %s: %s\n", name.c_str( ),
					warning.location.begin.line + 1, warning.location.begin.column + 1,
					Luau::LintWarning::getName( warning.code ), warning.text.c_str( ) );
			}
		}

		++checked;

		if ( reported != 0 ) {
			++failures;
		}
	}

	if ( failures == 0 ) {
		std::printf( "ok: %d extension file(s) type-check against %s\n", checked,
			definition_path.filename( ).string( ).c_str( ) );
	}

	return failures;
}
