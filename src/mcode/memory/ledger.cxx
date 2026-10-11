#include "mcode/memory/ledger.hxx"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/text.hxx"

namespace mcode::memory {

	namespace {

		constexpr std::string_view NOTES_DIRECTORY = "notes";
		constexpr std::string_view LEDGER_NAME = "notes.md";

		auto digest_of( const std::filesystem::path& workspace ) -> std::string {
			const auto canonical = std::filesystem::weakly_canonical( workspace );
			const auto folded = text::ascii_lower( canonical.generic_string( ) );

			return hash_bytes( folded );
		}

		auto today( ) -> std::string {
			const auto now = std::chrono::system_clock::now( );
			const auto seconds = std::chrono::system_clock::to_time_t( now );
			auto parts = std::tm{ };

		#if defined( _WIN32 )
			::gmtime_s( &parts, &seconds );
		#else
			::gmtime_r( &seconds, &parts );
		#endif

			auto out = std::ostringstream{ };
			out << ( parts.tm_year + 1900 ) << '-' << ( parts.tm_mon + 1 < 9 ? "0" : "" )
				<< ( parts.tm_mon + 1 ) << '-' << ( parts.tm_mday < 10 ? "0" : "" ) << parts.tm_mday;

			return out.str( );
		}

	}

	auto line_count( const std::string_view text ) noexcept -> std::size_t {
		if ( text.empty( ) ) {
			return 0;
		}

		auto count = std::size_t{ 1 };

		for ( const auto character : text ) {
			if ( character == '\n' ) {
				++count;
			}
		}

		return count;
	}

	auto ledger_path( const std::filesystem::path& workspace ) -> result< std::filesystem::path > {
		auto base = platform::app_data_path( platform::data_kind::data );

		if ( !base ) {
			return std::unexpected( base.error( ) );
		}

		return *base / NOTES_DIRECTORY / ( digest_of( workspace ) + "-" + std::string{ LEDGER_NAME } );
	}

	auto read_ledger( const std::filesystem::path& workspace ) -> result< std::string > {
		const auto path = ledger_path( workspace );

		if ( !path ) {
			return std::unexpected( path.error( ) );
		}

		auto stream = std::ifstream{ *path, std::ios::binary };

		if ( !stream ) {
			return std::string{ };
		}

		auto buffer = std::ostringstream{ };
		buffer << stream.rdbuf( );

		return buffer.str( );
	}

	auto append_note( const std::filesystem::path& workspace, const std::string_view text,
		const std::string_view source ) -> status {
		if ( text.empty( ) ) {
			return std::unexpected( fail( errc::config, "nothing to remember" ) );
		}

		const auto path = ledger_path( workspace );

		if ( !path ) {
			return std::unexpected( path.error( ) );
		}

		auto existing = read_ledger( workspace );

		if ( !existing ) {
			return std::unexpected( existing.error( ) );
		}

		auto entry = std::string{ "## " };
		entry += today( );
		entry += " — ";
		entry += source;
		entry += '\n';
		entry += text;

		if ( !entry.ends_with( '\n' ) ) {
			entry += '\n';
		}

		const auto lines = line_count( *existing ) + line_count( entry );

		if ( lines > MAX_NOTES_LINES ) {
			return std::unexpected( fail( errc::config,
				"the notes ledger would exceed " + std::to_string( MAX_NOTES_LINES )
					+ " lines; prune the oldest entries in " + path->string( ) ) );
		}

		auto error = std::error_code{ };
		std::filesystem::create_directories( path->parent_path( ), error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"cannot create " + path->parent_path( ).string( ) + ": " + error.message( ) ) );
		}

		auto stream = std::ofstream{ *path, std::ios::binary | std::ios::app };

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "cannot open " + path->string( ) ) );
		}

		stream << entry;

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "cannot write " + path->string( ) ) );
		}

		return { };
	}

}
