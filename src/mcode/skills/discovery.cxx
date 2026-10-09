#include "mcode/skills/discovery.hxx"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

#include "mcode/support/text.hxx"

namespace mcode::skills {

	namespace {

		inline constexpr std::size_t SKILL_NAME_MAX_LENGTH = 64;
		inline constexpr std::size_t SKILL_DESCRIPTION_MAX_LENGTH = 1024;
		inline constexpr std::uintmax_t FRONTMATTER_READ_BYTES = 4u * 1024u;
		inline constexpr std::uintmax_t SKILL_BODY_MAX_BYTES = 16u * 1024u;
		inline constexpr std::size_t NESTED_SCAN_DEPTH = 6;

		auto is_valid_name( const std::string_view name ) -> bool {
			if ( name.empty( ) || name.size( ) > SKILL_NAME_MAX_LENGTH ) {
				return false;
			}

			auto previous_was_dash = true;

			for ( const auto character : name ) {
				if ( character == '-' ) {
					if ( previous_was_dash ) {
						return false;
					}

					previous_was_dash = true;

					continue;
				}

				if ( ( character < 'a' || character > 'z' ) &&
					( character < '0' || character > '9' ) ) {
					return false;
				}

				previous_was_dash = false;
			}

			return !previous_was_dash;
		}

		auto head_of( const std::filesystem::path& file ) -> result< std::string > {
			auto stream = std::ifstream{ file, std::ios::binary };

			if ( !stream ) {
				return std::unexpected( fail( errc::io,
					"cannot open " + file.string( ) ) );
			}

			auto buffer = std::string( FRONTMATTER_READ_BYTES, '\0' );

			stream.read( buffer.data( ), static_cast< std::streamsize >( buffer.size( ) ) );
			buffer.resize( static_cast< std::size_t >( stream.gcount( ) ) );

			return buffer;
		}

		auto validate( const std::filesystem::path& directory, const skill_origin origin,
			discovery_report& report ) -> void {
			const auto file = directory / "SKILL.md";

			auto head = head_of( file );

			if ( !head ) {
				report.rejected.push_back( directory.string( ) + ": " + head.error( ).msg );

				return;
			}

			auto parsed = parse_frontmatter( *head );

			if ( !parsed ) {
				report.rejected.push_back(
					file.string( ) + ": " + parsed.error( ).msg );

				return;
			}

			if ( !is_valid_name( parsed->name ) ) {
				report.rejected.push_back( file.string( ) + ": 'name' is not a valid "
					"skill name" );

				return;
			}

			const auto directory_name = directory.filename( ).string( );

			if ( parsed->name != directory_name ) {
				report.rejected.push_back( file.string( ) + ": 'name' is '" +
					parsed->name + "' but the directory is '" + directory_name + "'" );

				return;
			}

			if ( parsed->description.empty( ) ) {
				report.rejected.push_back( file.string( ) + ": 'description' is empty" );

				return;
			}

			if ( parsed->description.size( ) > SKILL_DESCRIPTION_MAX_LENGTH ) {
				report.rejected.push_back( file.string( ) + ": 'description' exceeds " +
					std::to_string( SKILL_DESCRIPTION_MAX_LENGTH ) + " bytes" );

				return;
			}

			auto entry = skill_entry{ };
			entry.name = std::move( parsed->name );
			entry.description = std::move( parsed->description );
			entry.directory = directory;
			entry.file = file;
			entry.body_offset = parsed->body_offset;
			entry.origin = origin;
			entry.disable_model_invocation = parsed->disable_model_invocation;

			report.entries.push_back( std::move( entry ) );
		}

		auto scan_flat( const std::filesystem::path& root, const skill_origin origin,
			discovery_report& report ) -> void {
			auto error = std::error_code{ };
			const auto entries = std::filesystem::directory_iterator{ root, error };

			if ( error ) {
				return;
			}

			for ( const auto& entry : entries ) {
				if ( !entry.is_directory( error ) || error ) {
					continue;
				}

				validate( entry.path( ), origin, report );
			}
		}

		// Depth-bounded so a pathological tree cannot hang startup.
		template< typename Scan >
		auto scan_nested( const std::filesystem::path& directory, const std::size_t depth,
			Scan&& scan ) -> void {
			if ( depth > NESTED_SCAN_DEPTH ) {
				return;
			}

			auto error = std::error_code{ };
			const auto entries = std::filesystem::directory_iterator{ directory, error };

			if ( error ) {
				return;
			}

			for ( const auto& entry : entries ) {
				if ( !entry.is_directory( error ) || error ) {
					continue;
				}

				if ( entry.path( ).filename( ) == ".mcode" ) {
					scan( entry.path( ) / "skills", skill_origin::project );

					continue;
				}

				scan_nested( entry.path( ), depth + 1, scan );
			}
		}

		// Symlinked roots would otherwise index the same skill twice.
		auto canonical_key( const std::filesystem::path& path ) -> std::string {
			auto error = std::error_code{ };
			const auto resolved = std::filesystem::weakly_canonical( path, error );

			auto key = ( error ? path : resolved ).string( );

			#if defined( _WIN32 )
				std::transform( key.begin( ), key.end( ), key.begin( ),
					[]( const unsigned char character ) {
						return static_cast< char >( std::tolower( character ) );
					} );
			#endif

			return key;
		}

		auto origin_rank( const skill_origin origin ) noexcept -> int {
			switch ( origin ) {
				case skill_origin::project: return 0;
				case skill_origin::user: return 1;
				case skill_origin::extension: return 2;
			}

			return 3;
		}

	} // namespace

	auto to_string( const skill_origin origin ) noexcept -> std::string_view {
		switch ( origin ) {
			case skill_origin::project: return "project";
			case skill_origin::user: return "user";
			case skill_origin::extension: return "extension";
		}

		return "unknown";
	}

	auto discover_skills( const discovery_options& options ) -> discovery_report {
		auto report = discovery_report{ };

		// Deduped before validation: a skill scanned twice would be rejected twice.
		auto scanned = std::unordered_set< std::string >{ };

		auto scan = [ &scanned, &report ]( const std::filesystem::path& directory,
			const skill_origin origin ) {
			const auto key = canonical_key( directory );

			if ( scanned.contains( key ) ) {
				return;
			}

			scanned.insert( key );
			scan_flat( directory, origin, report );
		};

		if ( !options.workspace.empty( ) ) {
			scan( options.workspace / ".mcode" / "skills", skill_origin::project );
			scan_nested( options.workspace, 0, scan );
		}

		if ( !options.user_root.empty( ) ) {
			scan( options.user_root, skill_origin::user );
		}

		for ( const auto& root : options.extension_roots ) {
			scan( root / "skills", skill_origin::extension );
		}

		// The highest-precedence origin wins; no second index line exists for the losers.
		std::sort( report.entries.begin( ), report.entries.end( ),
			[]( const skill_entry& left, const skill_entry& right ) {
				if ( left.name != right.name ) {
					return left.name < right.name;
				}

				return origin_rank( left.origin ) < origin_rank( right.origin );
			} );

		auto seen_paths = std::unordered_set< std::string >{ };
		auto deduped = std::vector< skill_entry >{ };
		auto keep = std::size_t{ 0 };

		while ( keep < report.entries.size( ) ) {
			const auto& entry = report.entries[ keep ];

			if ( seen_paths.contains( canonical_key( entry.directory ) ) ) {
				++keep;

				continue;
			}

			seen_paths.insert( canonical_key( entry.directory ) );
			deduped.push_back( entry );

			auto next = keep + 1;

			while ( next < report.entries.size( ) &&
				report.entries[ next ].name == entry.name ) {
				++next;
			}

			keep = next;
		}

		report.entries = std::move( deduped );

		return report;
	}

	auto read_skill_body( const std::vector< skill_entry >& entries,
		const std::string_view name ) -> result< std::string > {
		const auto found = std::find_if( entries.begin( ), entries.end( ),
			[ & ]( const skill_entry& entry ) { return entry.name == name; } );

		if ( found == entries.end( ) ) {
			return std::unexpected( fail( errc::config,
				"no skill named '" + std::string{ name } + "'" ) );
		}

		auto stream = std::ifstream{ found->file, std::ios::binary };

		if ( !stream ) {
			return std::unexpected( fail( errc::io,
				"cannot open " + found->file.string( ) ) );
		}

		auto buffer = std::string( SKILL_BODY_MAX_BYTES, '\0' );

		stream.read( buffer.data( ), static_cast< std::streamsize >( buffer.size( ) ) );
		buffer.resize( static_cast< std::size_t >( stream.gcount( ) ) );

		if ( found->body_offset < buffer.size( ) ) {
			buffer.erase( 0, found->body_offset );
		} else {
			buffer.clear( );
		}

		return buffer;
	}

}
