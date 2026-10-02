#include "mcode/perm/store.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <utility>

#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

namespace mcode::perm {

	namespace {

		struct section_spec {
			std::string_view key;
		};

		inline constexpr auto SECTIONS = std::array< section_spec, 3 >{
			section_spec{ "exec" },
			section_spec{ "paths" },
			section_spec{ "tools" },
		};

		[[nodiscard]] auto find_section( store_layer& layer, const std::string_view key )
			-> std::map< std::string, store_decision, std::less<> >* {
			if ( key == "exec" ) {
				return &layer.exec;
			}

			if ( key == "paths" ) {
				return &layer.paths;
			}

			if ( key == "tools" ) {
				return &layer.tools;
			}

			return nullptr;
		}

		[[nodiscard]] auto parse_decision( const std::string_view text )
			-> std::optional< store_decision > {
			if ( text == "allow" ) {
				return store_decision::allow;
			}

			if ( text == "deny" ) {
				return store_decision::deny;
			}

			return std::nullopt;
		}

		[[nodiscard]] auto decision_text( const store_decision value ) -> std::string_view {
			return value == store_decision::allow ? "allow" : "deny";
		}

		[[nodiscard]] auto read_json( const std::filesystem::path& file )
			-> result< std::optional< json::node > > {
			auto input = std::ifstream{ platform::to_extended_path( file ), std::ios::binary };

			if ( !input ) {
				return std::optional< json::node >{ };
			}

			auto text = std::string{ };
			auto line = std::string{ };

			while ( std::getline( input, line ) ) {
				text += line;
				text += '\n';
			}

			if ( text.empty( ) ) {
				return std::optional< json::node >{ };
			}

			auto parsed = json::node_from_json( text );

			if ( !parsed ) {
				return std::unexpected( fail( errc::json,
					file.string( ) + ": " + parsed.error( ).msg ) );
			}

			return std::optional< json::node >{ std::move( *parsed ) };
		}

		// rendered by hand: the JSON writer's setters all refuse an empty key.
		[[nodiscard]] auto render( const store_layer& layer ) -> std::string {
			auto out = std::string{ "{\n\t\"version\": " };
			out += std::to_string( STORE_VERSION );

			for ( const auto& spec : SECTIONS ) {
				const auto* entries = find_section( const_cast< store_layer& >( layer ), spec.key );

				if ( entries == nullptr || entries->empty( ) ) {
					continue;
				}

				out += ",\n\t\"";
				out += spec.key;
				out += "\": {";

				auto first = true;

				for ( const auto& [ key, decision ] : *entries ) {
					if ( !first ) {
						out += ',';
					}

					first = false;
					out += "\n\t\t\"";
					json::append_escaped( out, key );
					out += "\": \"";
					out += decision_text( decision );
					out += '"';
				}

				out += "\n\t}";
			}

			out += "\n}";

			return out;
		}

	}

	auto canonical_argv( const std::vector< std::string >& argv ) -> std::string {
		auto out = std::string{ };

		for ( const auto& token : argv ) {
			if ( !out.empty( ) ) {
				out += ' ';
			}

			out += token;
		}

		return out;
	}

	auto canonical_path_key( const std::filesystem::path& path )
		-> std::optional< std::string > {
		auto canonical = platform::canonicalize( path );

		if ( !canonical ) {
			return std::nullopt;
		}

		auto text = canonical->generic_string( );

		if ( platform::case_insensitive_paths( ) ) {
			auto lowered = std::string{ };

			for ( const auto character : text ) {
				lowered += character >= 'A' && character <= 'Z'
					? static_cast< char >( character - 'A' + 'a' )
					: character;
			}

			text = std::move( lowered );
		}

		return text;
	}

	remember_store::remember_store( std::filesystem::path file ) : file_( std::move( file ) ) { }

	auto remember_store::load( ) -> result< std::optional< store_layer > > {
		auto content = read_json( file_ );

		if ( !content ) {
			return std::unexpected( content.error( ) );
		}

		if ( !content->has_value( ) ) {
			return std::optional< store_layer >{ };
		}

		auto layer = store_layer{ };

		if ( ( *content )->type != json::node::kind::object ) {
			return std::unexpected( fail( errc::json,
				file_.string( ) + ": the store root must be an object" ) );
		}

		for ( const auto& spec : SECTIONS ) {
			const auto section = ( *content )->member( spec.key );

			if ( section == nullptr ) {
				continue;
			}

			if ( section->type != json::node::kind::object ) {
				return std::unexpected( fail( errc::json,
					file_.string( ) + ": '" + std::string{ spec.key } +
					"' must be an object of decision strings" ) );
			}

			auto* entries = find_section( layer, spec.key );

			for ( const auto& [ key, value ] : section->members ) {
				if ( value.type != json::node::kind::string ) {
					return std::unexpected( fail( errc::json,
						file_.string( ) + ": '" + std::string{ spec.key } + "/" + key +
						"' must be \"allow\" or \"deny\"" ) );
				}

				const auto decision = parse_decision( value.text );

				if ( !decision ) {
					return std::unexpected( fail( errc::json,
						file_.string( ) + ": '" + std::string{ spec.key } + "/" + key +
						"' is '" + value.text + "'; only \"allow\" and \"deny\" are stored" ) );
				}

				entries->insert_or_assign( key, *decision );
			}
		}

		return std::optional< store_layer >{ std::move( layer ) };
	}

	auto remember_store::save( const store_layer& additions ) -> status {
		// load-merge-write: two answers in one session must both survive.
		auto existing = load( );

		if ( !existing ) {
			return std::unexpected( existing.error( ) );
		}

		auto merged = store_layer{ };

		if ( existing->has_value( ) ) {
			merged = **existing;
		}

		for ( const auto& spec : SECTIONS ) {
			const auto* source = find_section( const_cast< store_layer& >( additions ), spec.key );

			if ( source == nullptr ) {
				continue;
			}

			auto* target = find_section( merged, spec.key );

			for ( const auto& [ key, decision ] : *source ) {
				target->insert_or_assign( key, decision );
			}
		}

		const auto text = render( merged );

		auto error_code = std::error_code{ };
		const auto parent = file_.parent_path( );

		if ( !parent.empty( ) ) {
			std::filesystem::create_directories( platform::to_extended_path( parent ),
				error_code );

			if ( error_code ) {
				return std::unexpected( fail( errc::io,
					"cannot create " + parent.string( ) + ": " + error_code.message( ) ) );
			}
		}

		// a sibling temp file, so the rename is same-filesystem and atomic.
		static auto counter = std::atomic< std::uint64_t >{ 0 };

		const auto stamp = std::to_string( support::epoch_milliseconds( ) ) + "-" +
			std::to_string( counter.fetch_add( 1, std::memory_order_relaxed ) );

		auto temporary = file_;
		temporary += ".mcode-tmp-" + stamp;

		{
			auto output = std::ofstream{ platform::to_extended_path( temporary ),
				std::ios::binary | std::ios::trunc };

			if ( !output ) {
				return std::unexpected( fail( errc::io,
					"cannot open " + temporary.string( ) ) );
			}

			output.write( text.data( ), static_cast< std::streamsize >( text.size( ) ) );
			output.close( );

			if ( !output ) {
				std::filesystem::remove( platform::to_extended_path( temporary ), error_code );

				return std::unexpected( fail( errc::io,
					"failed to write " + temporary.string( ) ) );
			}
		}

		std::filesystem::rename( platform::to_extended_path( temporary ),
			platform::to_extended_path( file_ ), error_code );

		if ( error_code ) {
			std::filesystem::remove( platform::to_extended_path( temporary ), error_code );
			error_code.clear( );

			return std::unexpected( fail( errc::io,
				"cannot replace " + file_.string( ) + " with its temp file" ) );
		}

		return { };
	}

	[[nodiscard]] auto store_key_for( const request_identity& request )
		-> std::optional< store_key > {
		if ( request.resource.empty( ) && request.klass == tool_class::exec ) {
			// an unparsable command has no canonical form to remember.
			return std::nullopt;
		}

		if ( request.klass == tool_class::exec ) {
			return store_key{ store_section::exec, request.resource };
		}

		if ( request.klass == tool_class::read || request.klass == tool_class::write ) {
			// the user's own `paths` entries are hand-written globs, never auto-persisted.
			return std::nullopt;
		}

		if ( request.tool_name.empty( ) ) {
			return std::nullopt;
		}

		return store_key{ store_section::tools, request.tool_name };
	}

	[[nodiscard]] auto section_of( store_layer& layer, const store_section section )
		-> store_map& {
		switch ( section ) {
			case store_section::exec: return layer.exec;
			case store_section::paths: return layer.paths;
			case store_section::tools: return layer.tools;
		}

		return layer.tools;
	}

	[[nodiscard]] auto section_of( const store_layer& layer, const store_section section )
		-> const store_map& {
		switch ( section ) {
			case store_section::exec: return layer.exec;
			case store_section::paths: return layer.paths;
			case store_section::tools: return layer.tools;
		}

		return layer.tools;
	}

	auto load_store_into( remember_store* store, const mcode::workspace& space,
		store_layer& into, std::vector< std::string >& warnings ) -> status {
		if ( store == nullptr ) {
			return { };
		}

		const auto loaded = store->load( );

		if ( !loaded ) {
			return std::unexpected( loaded.error( ) );
		}

		if ( !loaded->has_value( ) ) {
			return { };
		}

		// merged rather than assigned, so a later layer wins on a key collision.
		for ( const auto& spec : SECTIONS ) {
			const auto* source = find_section( const_cast< store_layer& >( **loaded ), spec.key );

			if ( source == nullptr ) {
				continue;
			}

			auto* target = find_section( into, spec.key );

			for ( const auto& [ key, decision ] : *source ) {
				target->insert_or_assign( key, decision );
			}
		}

		// a project-scoped store cannot widen: drop every allow it carries.
		const auto store_text = store->file( ).generic_string( );
		const auto root_text = space.root( ).generic_string( );

		if ( !store_text.starts_with( root_text + "/" ) &&
			!store_text.starts_with( root_text + "\\" ) ) {
			return { };
		}

		auto dropped = std::size_t{ 0 };

		const auto filter = [ &dropped ]( auto& entries ) {
			for ( auto entry = entries.begin( ); entry != entries.end( ); ) {
				if ( entry->second == store_decision::allow ) {
					entry = entries.erase( entry );
					++dropped;

					continue;
				}

				++entry;
			}
		};

		filter( into.exec );
		filter( into.paths );
		filter( into.tools );

		if ( dropped > 0 ) {
			warnings.push_back( "project store " + store_text +
				" cannot widen; dropped " + std::to_string( dropped ) +
				" allow entries" );
		}

		return { };
	}

}
