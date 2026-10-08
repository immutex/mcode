#include "setup_detail.hxx"

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/platform/seams.hxx"

namespace mcode::cli::detail {

	// Only the models the compiled-in table prices. A model outside it needs
	// a `[models."id"]` block, which the wizard names rather than silently
	// writing a config that would refuse to run.
	[[nodiscard]] auto suggested_models( const std::size_t provider_index )
		-> std::vector< std::string > {
		switch ( provider_index ) {
			case 0: return { "gpt-5", "gpt-5-mini" };
			default: return { };
		}
	}

	// -------------------------------------------------------------------
	// Config writing. The file is edited as text: the `[model]` section is
	// replaced or appended and everything else, including the
	// `[models."..."]` pricing blocks and their comments, is preserved byte
	// for byte. Parsing and re-serialising would need a TOML writer that
	// does not exist, and would drop the rationale those comments carry.
	// -------------------------------------------------------------------

	[[nodiscard]] auto escape_toml( const std::string_view text ) -> std::string {
		auto out = std::string{ };
		out.reserve( text.size( ) );

		for ( const auto character : text ) {
			switch ( character ) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;

				default: out.push_back( character ); break;
			}
		}

		return out;
	}

	[[nodiscard]] auto render_section( const model_settings& settings ) -> std::string {
		auto out = std::string{ };
		out += '[';
		out += SECTION_NAME;
		out += ']';
		out += "\nprovider = \"";
		out += escape_toml( settings.provider );
		out += "\"\nmodel = \"";
		out += escape_toml( settings.model );
		out += "\"\n";

		if ( !settings.base_url.empty( ) ) {
			out += "base_url = \"";
			out += escape_toml( settings.base_url );
			out += "\"\n";
		}

		out += "api_key_env = \"";
		out += escape_toml( settings.api_key_env );
		out += "\"\n";

		return out;
	}

	// Replaces the `[model]` section in place, or appends one.
	//
	// Line-based rather than offset-based, because a config edited on Windows
	// carries CRLF endings and a check for a bare `\n` after the header would
	// miss it and append a SECOND `[model]` section. Two sections with the
	// same name are a duplicate-key error at load, so that failure would land
	// on the user's next start rather than here.
	[[nodiscard]] auto splice_section( const std::string_view existing,
		const std::string& section ) -> std::string {
		// Split preserving nothing; the ending is reapplied from the original
		// dominant style so the file is not half-converted.
		auto lines = std::vector< std::string >{ };
		auto endings = std::string{ };
		auto cursor = std::size_t{ 0 };

		while ( cursor <= existing.size( ) && !existing.empty( ) ) {
			const auto next = existing.find( '\n', cursor );

			if ( next == std::string_view::npos ) {
				if ( cursor < existing.size( ) ) {
					lines.emplace_back( existing.substr( cursor ) );
				}

				break;
			}

			auto line = std::string{ existing.substr( cursor, next - cursor ) };

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.pop_back( );

				if ( endings.empty( ) ) {
					endings = "\r\n";
				}
			} else if ( endings.empty( ) ) {
				endings = "\n";
			}

			lines.push_back( std::move( line ) );
			cursor = next + 1;
		}

		if ( endings.empty( ) ) {
			endings = "\n";
		}

		// The replacement, split the same way so it joins the same file.
		auto replacement = std::vector< std::string >{ };
		auto section_cursor = std::size_t{ 0 };

		while ( section_cursor <= section.size( ) ) {
			const auto next = section.find( '\n', section_cursor );

			if ( next == std::string::npos ) {
				break;
			}

			replacement.emplace_back( section.substr( section_cursor, next - section_cursor ) );
			section_cursor = next + 1;
		}

		// A TOML table header, ignoring the indentation, trailing space and
		// trailing comment that TOML allows. Exact equality missed
		// `[model] # my provider`, so the append branch ran and wrote a
		// SECOND `[model]` -- which the loader then rejects as a duplicate
		// key, leaving the user's config unreadable until they hand-edit it.
		const auto header_name = []( const std::string& text )
			-> std::optional< std::string > {
			auto header_start = std::size_t{ 0 };

			while ( header_start < text.size( ) &&
				( text[ header_start ] == ' ' || text[ header_start ] == '\t' ) ) {
				++header_start;
			}

			if ( header_start >= text.size( ) || text[ header_start ] != '[' ) {
				return std::nullopt;
			}

			const auto close = text.find( ']', header_start + 1 );

			if ( close == std::string::npos ) {
				return std::nullopt;
			}

			const auto after = text.find_first_not_of( " \t", close + 1 );

			if ( after != std::string::npos && text[ after ] != '#' ) {
				return std::nullopt;
			}

			return text.substr( header_start + 1, close - header_start - 1 );
		};

		const auto is_header = [ & ]( const std::string& line ) -> bool {
			return header_name( line ).has_value( );
		};

		const auto start = [&]( ) -> std::size_t {
			for ( auto index = std::size_t{ 0 }; index < lines.size( ); ++index ) {
				const auto name = header_name( lines[ index ] );

				if ( name.has_value( ) && *name == SECTION_NAME ) {
					return index;
				}
			}

			return lines.size( );
		}( );

		if ( start == lines.size( ) ) {
			// Not present: append, with a blank separator when the file is not empty.
			if ( !lines.empty( ) && !lines.back( ).empty( ) ) {
				lines.emplace_back( std::string{ } );
			}

			lines.insert( lines.end( ), replacement.begin( ), replacement.end( ) );
		} else {
			// The section runs to the next header, or to the end of the file.
			auto end = start + 1;

			while ( end < lines.size( ) && !is_header( lines[ end ] ) ) {
				++end;
			}

			lines.erase( lines.begin( ) + static_cast< std::ptrdiff_t >( start ),
				lines.begin( ) + static_cast< std::ptrdiff_t >( end ) );
			lines.insert( lines.begin( ) + static_cast< std::ptrdiff_t >( start ),
				replacement.begin( ), replacement.end( ) );
		}

		auto out = std::string{ };

		for ( const auto& line : lines ) {
			out += line;
			out += endings;
		}

		return out;
	}

	[[nodiscard]] auto read_text_file( const std::filesystem::path& path )
		-> std::optional< std::string > {
		if ( !std::filesystem::exists( path ) ) {
			return std::string{ };
		}

		auto* file = std::fopen( path.string( ).c_str( ), "rb" );

		if ( file == nullptr ) {
			return std::nullopt;
		}

		auto out = std::string{ };
		auto buffer = std::array< char, 4096 >{ };
		auto count = std::size_t{ 0 };

		while ( ( count = std::fread( buffer.data( ), 1, buffer.size( ), file ) ) > 0 ) {
			out.append( buffer.data( ), count );
		}

		std::fclose( file );

		return out;
	}

	// Writes beside the target and renames, so an interrupted run cannot
	// leave a half-written config that the next start would refuse.
	[[nodiscard]] auto write_text_file( const std::filesystem::path& path,
		const std::string_view content ) -> bool {
		auto error = std::error_code{ };

		if ( !path.parent_path( ).empty( ) ) {
			std::filesystem::create_directories( path.parent_path( ), error );

			if ( error ) {
				return false;
			}
		}

		const auto temporary = path.string( ) + ".new";
		auto* file = std::fopen( temporary.c_str( ), "wb" );

		if ( file == nullptr ) {
			return false;
		}

		const auto written = std::fwrite( content.data( ), 1, content.size( ), file );
		std::fclose( file );

		if ( written != content.size( ) ) {
			std::remove( temporary.c_str( ) );

			return false;
		}

		std::filesystem::rename( temporary, path, error );

		if ( !error ) {
			return true;
		}

		// A rename onto a file that is open fails on Windows; a copy still
		// completes the write rather than losing the user's settings.
		std::filesystem::copy_file( temporary, path,
			std::filesystem::copy_options::overwrite_existing, error );
		std::remove( temporary.c_str( ) );

		return !error;
	}

	[[nodiscard]] auto config_path( ) -> std::filesystem::path {
		const auto directory = mcode::platform::app_data_path(
			mcode::platform::data_kind::config );

		if ( !directory ) {
			return std::filesystem::path{ };
		}

		return *directory / CONFIG_FILE_NAME;
	}

	// -------------------------------------------------------------------
	// Verification. The strongest check available is the one the user is
	// about to run: write the config, then run a real turn with it. That
	// exercises the descriptor, the endpoint, the credential and the
	// streaming parser, so a success here is the whole path working rather
	// than a hand-rolled request that might not match it.
	// -------------------------------------------------------------------

	[[nodiscard]] auto self_path( ) -> std::filesystem::path {
		const auto directory = mcode::platform::executable_directory( );

		if ( !directory ) {
			return std::filesystem::path{ };
		}

		for ( const auto name : { "mcode", "mcode.exe" } ) {
			const auto candidate = *directory / name;

			if ( std::filesystem::exists( candidate ) ) {
				return candidate;
			}
		}

		return { };
	}

}
