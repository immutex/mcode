#include "exec_internal.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"

namespace mcode::cli::detail {

	auto write_line( const std::string_view text ) -> void {
		std::fwrite( text.data( ), 1, text.size( ), stdout );
		std::fputc( '\n', stdout );
	}

	auto to_utf8( const std::filesystem::path& path ) -> std::string {
		const auto text = path.generic_u8string( );

		return std::string{ reinterpret_cast< const char* >( text.data( ) ), text.size( ) };
	}

	auto workspace_digest( const std::filesystem::path& canonical_root ) -> std::string {
		return mcode::hash_bytes( to_utf8( canonical_root ) );
	}

	auto root_for_digest( const std::filesystem::path& workspace_root )
		-> std::filesystem::path {
		auto canonical = platform::canonicalize( workspace_root );

		if ( canonical ) {
			return *canonical;
		}

		auto error = std::error_code{ };
		auto absolute = std::filesystem::absolute( workspace_root, error );

		if ( error ) {
			return workspace_root.lexically_normal( );
		}

		return absolute.lexically_normal( );
	}

	auto session_file_name( const std::string_view id ) -> std::string {
		return std::string{ id } + std::string{ SESSION_FILE_SUFFIX };
	}

	auto padded_stamp( const std::int64_t start_ms ) -> std::string {
		auto text = std::to_string( start_ms );

		if ( text.size( ) < SESSION_STAMP_LENGTH ) {
			text.insert( 0, SESSION_STAMP_LENGTH - text.size( ), '0' );
		}

		return text;
	}

	auto is_session_id( const std::string_view id ) -> bool {
		if ( id.size( ) != SESSION_ID_LENGTH || id[ SESSION_DIGEST_LENGTH ] != '-' ) {
			return false;
		}

		for ( auto index = std::size_t{ 0 }; index < id.size( ); ++index ) {
			if ( index == SESSION_DIGEST_LENGTH ) {
				continue;
			}

			const auto character = id[ index ];
			const auto is_hex = ( character >= '0' && character <= '9' )
				|| ( character >= 'a' && character <= 'f' );

			if ( !is_hex ) {
				return false;
			}
		}

		return true;
	}

	auto stamp_of_session_id( const std::string_view id ) -> std::int64_t {
		const auto digits = id.substr( SESSION_DIGEST_LENGTH + 1 );
		auto value = std::int64_t{ 0 };
		const auto parsed = std::from_chars( digits.data( ), digits.data( ) + digits.size( ),
			value );

		if ( parsed.ec != std::errc{ } ) {
			return 0;
		}

		return value;
	}

	auto scan_sessions( const std::filesystem::path& directory )
		-> result< std::vector< session_ref > > {
		auto out = std::vector< session_ref >{ };
		auto error = std::error_code{ };

		// A workspace with no session yet is the normal first run, not a failure.
		if ( !std::filesystem::is_directory( directory, error ) || error ) {
			return out;
		}

		auto iterator = std::filesystem::directory_iterator{ directory, error };

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"could not read " + directory.string( ) + ": " + error.message( ) ) );
		}

		for ( const auto& entry : iterator ) {
			// A fresh code per call: a stale one would skip every later entry.
			auto entry_error = std::error_code{ };

			if ( !entry.is_regular_file( entry_error ) || entry_error ) {
				continue;
			}

			const auto name = to_utf8( entry.path( ).filename( ) );

			if ( !name.ends_with( SESSION_FILE_SUFFIX ) ) {
				continue;
			}

			const auto id = name.substr( 0, name.size( ) - SESSION_FILE_SUFFIX.size( ) );

			if ( !is_session_id( id ) ) {
				continue;
			}

			auto session = session_ref{ };
			session.path = entry.path( );
			session.id = id;
			session.started_ms = stamp_of_session_id( id );

			const auto size = std::filesystem::file_size( session.path, entry_error );

			session.size_bytes = entry_error ? 0 : size;

			out.push_back( std::move( session ) );
		}

		// Newest first: the id carries the start time, so the ordering is deterministic
		// even when two runs land in the same millisecond.
		std::sort( out.begin( ), out.end( ),
			[]( const session_ref& left, const session_ref& right ) {
				if ( left.started_ms != right.started_ms ) {
					return left.started_ms > right.started_ms;
				}

				return left.id > right.id;
			} );

		return out;
	}

	auto format_utc( const std::int64_t milliseconds ) -> std::string {
		const auto seconds = static_cast< std::time_t >( milliseconds / 1000 );
		auto utc = std::tm{ };

	#if defined( _WIN32 )
		gmtime_s( &utc, &seconds );
	#else
		gmtime_r( &seconds, &utc );
	#endif

		auto buffer = std::array< char, 32 >{ };
		std::strftime( buffer.data( ), buffer.size( ), "%Y-%m-%dT%H:%M:%SZ", &utc );

		return buffer.data( );
	}


	auto resolve_approval_policy( const exec_options& options,
		const mcode::config::merged_config& config, const bool interactive )
		-> mcode::perm::permission_engine::options {
		auto resolved = mcode::perm::permission_engine::options{ };
		resolved.yolo = options.yolo;
		resolved.headless = !interactive;
		resolved.plan_mode = options.plan;

		// The permissive default is for a human who can see the disclaimer and use
		// /undo. A headless run has neither, so it keeps the engine's conservative
		// default and fails closed on anything the rules do not already allow.
		resolved.approval = options.approval.empty( )
			? config.get_string( "sandbox.approval" ).value_or(
				std::string{ interactive ? "never" : "on-request" } )
			: options.approval;

		// --yolo forces the permissive mode; --ask is applied after it so the
		// explicit request to be prompted wins over every permissive flag.
		if ( options.yolo ) {
			resolved.approval = "never";
		}

		if ( options.ask ) {
			resolved.approval = "on-request";
			resolved.yolo = false;
		}

		return resolved;
	}
}
