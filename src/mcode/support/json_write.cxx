#include "mcode/support/json.hxx"

#include "mcode/support/json_internal.hxx"

#include <yyjson.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mcode::json {

	namespace {

		// Converts a parsed value into the mutable tree. Used by `set_json` so a
		// pre-rendered schema or argument blob is embedded as parsed structure
		// rather than as a re-encoded string.
		auto from_val( yyjson_val* source ) -> result< node > {
			if ( source == nullptr ) {
				return std::unexpected( fail( errc::json, "null value" ) );
			}

			auto out = node{ };

			if ( yyjson_is_null( source ) ) {
				out.type = node::kind::null_value;

				return out;
			}

			if ( yyjson_is_bool( source ) ) {
				out.type = node::kind::boolean;
				out.boolean = yyjson_get_bool( source );

				return out;
			}

			if ( yyjson_is_sint( source ) ) {
				out.type = node::kind::integer;
				out.integer = yyjson_get_sint( source );

				return out;
			}

			if ( yyjson_is_uint( source ) ) {
				const auto wide = yyjson_get_uint( source );

				if ( wide > static_cast< std::uint64_t >( std::numeric_limits< std::int64_t >::max( ) ) ) {
					return std::unexpected( fail( errc::json,
						"integer is too large for a 64-bit signed integer" ) );
				}

				out.type = node::kind::integer;
				out.integer = static_cast< std::int64_t >( wide );

				return out;
			}

			if ( yyjson_is_real( source ) ) {
				out.type = node::kind::real;
				out.real = yyjson_get_real( source );

				return out;
			}

			if ( yyjson_is_str( source ) ) {
				out.type = node::kind::string;
				out.text = std::string{ yyjson_get_str( source ), yyjson_get_len( source ) };

				return out;
			}

			if ( yyjson_is_arr( source ) ) {
				out.type = node::kind::array;

				auto index = std::size_t{ 0 };
				auto count = std::size_t{ 0 };
				yyjson_val* item = nullptr;

				yyjson_arr_foreach( source, index, count, item ) {
					auto converted = from_val( item );

					if ( !converted ) {
						return std::unexpected( converted.error( ) );
					}

					out.items.push_back( std::move( *converted ) );
				}

				return out;
			}

			if ( yyjson_is_obj( source ) ) {
				out.type = node::kind::object;

				auto iterator = yyjson_obj_iter{ };

				if ( !yyjson_obj_iter_init( source, &iterator ) ) {
					return std::unexpected( fail( errc::json, "cannot iterate the object" ) );
				}

				while ( auto* name = yyjson_obj_iter_next( &iterator ) ) {
					if ( !yyjson_is_str( name ) ) {
						return std::unexpected( fail( errc::json, "object key is not a string" ) );
					}

					auto converted = from_val( yyjson_obj_iter_get_val( name ) );

					if ( !converted ) {
						return std::unexpected( converted.error( ) );
					}

					out.members.emplace( std::string{ yyjson_get_str( name ), yyjson_get_len( name ) },
						std::move( *converted ) );
				}

				return out;
			}

			return std::unexpected( fail( errc::json, "unsupported JSON value" ) );
		}

		// Builds one value in `doc`. Recursive, so a scalar, an array and an object
		// share one code path instead of three that drift.
		//
		// The caller decides where the result goes: `yyjson_mut_obj_add_val` for a
		// member, `yyjson_mut_arr_add_val` for an element.
		auto to_val( yyjson_mut_doc* doc, const node& source ) -> yyjson_mut_val* {
			switch ( source.type ) {
				case node::kind::null_value:
					return yyjson_mut_null( doc );

				case node::kind::boolean:
					return yyjson_mut_bool( doc, source.boolean );

				case node::kind::integer:
					return yyjson_mut_sint( doc, source.integer );

				case node::kind::real:
					return yyjson_mut_real( doc, source.real );

				case node::kind::string:
					return yyjson_mut_strncpy( doc, source.text.data( ), source.text.size( ) );

				case node::kind::array: {
					auto* array = yyjson_mut_arr( doc );

					if ( array == nullptr ) {
						return nullptr;
					}

					for ( const auto& item : source.items ) {
						auto* element = to_val( doc, item );

						if ( element == nullptr || !yyjson_mut_arr_add_val( array, element ) ) {
							return nullptr;
						}
					}

					return array;
				}

				case node::kind::object: {
					auto* object = yyjson_mut_obj( doc );

					if ( object == nullptr ) {
						return nullptr;
					}

					for ( const auto& [ key, member ] : source.members ) {
						auto* value = to_val( doc, member );

						if ( value == nullptr ||
							!yyjson_mut_obj_add_val( doc, object, key.c_str( ), value ) ) {
							return nullptr;
						}
					}

					return object;
				}
			}

			return nullptr;
		}

		// Every setter validates the same two preconditions before it touches the
		// tree, so they live in one place rather than seven.
		auto check_settable( const bool mutable_document, const std::string_view key,
			const std::string_view what ) -> status {
			if ( !mutable_document ) {
				return std::unexpected( fail( errc::json,
					std::string{ what } + " on a non-mutable document" ) );
			}

			if ( key.empty( ) ) {
				return std::unexpected( fail( errc::json, "empty key" ) );
			}

			return { };
		}

	}

	auto document::set_string( const std::string_view key, const std::string_view text ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_string" ); !ready ) {
			return ready;
		}

		auto entry = node{ };
		entry.type = node::kind::string;
		entry.text = std::string{ text };

		root_.members[ std::string{ key } ] = std::move( entry );

		return { };
	}

	auto document::set_int( const std::string_view key, const std::int64_t number ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_int" ); !ready ) {
			return ready;
		}

		auto entry = node{ };
		entry.type = node::kind::integer;
		entry.integer = number;

		root_.members[ std::string{ key } ] = std::move( entry );

		return { };
	}

	auto document::set_bool( const std::string_view key, const bool value ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_bool" ); !ready ) {
			return ready;
		}

		auto entry = node{ };
		entry.type = node::kind::boolean;
		entry.boolean = value;

		root_.members[ std::string{ key } ] = std::move( entry );

		return { };
	}

	auto document::set_real( const std::string_view key, const double value ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_real" ); !ready ) {
			return ready;
		}

		auto entry = node{ };
		entry.type = node::kind::real;
		entry.real = value;

		root_.members[ std::string{ key } ] = std::move( entry );

		return { };
	}

	auto document::set_node( const std::string_view key, node value ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_node" ); !ready ) {
			return ready;
		}

		root_.members[ std::string{ key } ] = std::move( value );

		return { };
	}

	auto document::append( node value ) -> status {
		if ( !mutable_ ) {
			return std::unexpected( fail( errc::json, "append on a non-mutable document" ) );
		}

		if ( root_.type != node::kind::array ) {
			return std::unexpected( fail( errc::json, "append on a document whose root is not an array" ) );
		}

		root_.items.push_back( std::move( value ) );

		return { };
	}

	namespace {

		// `insert_or_assign` would overwrite an existing scalar before any type check
		// ran, turning a refusal into a silent replacement. So the existing entry is
		// examined FIRST and only an absent key is created.
		auto container_at( node& root, const bool mutable_document, const std::string_view key,
			const node::kind wanted ) -> node* {
			if ( !mutable_document || key.empty( ) ) {
				return nullptr;
			}

			const auto found = root.members.find( key );

			if ( found != root.members.end( ) ) {
				return ( found->second.type == wanted ) ? &found->second : nullptr;
			}

			auto entry = node{ };
			entry.type = wanted;

			return &root.members.emplace( std::string{ key }, std::move( entry ) ).first->second;
		}

	}

	auto document::make_object_at( const std::string_view key ) -> node* {
		return container_at( root_, mutable_, key, node::kind::object );
	}

	auto document::make_array_at( const std::string_view key ) -> node* {
		return container_at( root_, mutable_, key, node::kind::array );
	}

	auto document::set_json( const std::string_view key, const std::string_view text ) -> status {
		if ( auto ready = check_settable( mutable_, key, "set_json" ); !ready ) {
			return ready;
		}

		auto parsed = document::parse( text );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		auto converted = from_val( yyjson_doc_get_root( parsed->doc_ ) );

		if ( !converted ) {
			return std::unexpected( converted.error( ) );
		}

		root_.members[ std::string{ key } ] = std::move( *converted );

		return { };
	}

	auto document::size( ) const noexcept -> std::size_t {
		if ( mutable_ ) {
			return root_.members.size( );
		}

		if ( doc_ == nullptr ) {
			return 0;
		}

		auto* root = yyjson_doc_get_root( doc_ );

		return ( root != nullptr && yyjson_is_obj( root ) ) ? yyjson_obj_size( root ) : 0;
	}

	auto document::dump( const bool pretty ) const -> result< std::string > {
		if ( mutable_ ) {
			auto doc = mut_doc_pointer{ yyjson_mut_doc_new( nullptr ), detail::free_mut_doc };

			if ( !doc ) {
				return std::unexpected( fail( errc::json, "yyjson_mut_doc_new failed" ) );
			}

			auto* root = to_val( doc.get( ), root_ );

			if ( root == nullptr ) {
				return std::unexpected( fail( errc::json, "failed to build the document" ) );
			}

			yyjson_mut_doc_set_root( doc.get( ), root );

			auto write_error = yyjson_write_err{ };
			const auto raw = detail::owned_cstr{
				yyjson_mut_write_opts( doc.get( ), detail::write_flags( pretty ), nullptr, nullptr, &write_error ) };

			if ( !raw ) {
				return std::unexpected(
					fail( errc::json, write_error.msg != nullptr ? write_error.msg : "write error" ) );
			}

			return std::string{ raw.get( ) };
		}

		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "dump on an empty document" ) );
		}

		auto write_error = yyjson_write_err{ };
		const auto raw =
			detail::owned_cstr{ yyjson_write_opts( doc_, detail::write_flags( pretty ), nullptr, nullptr, &write_error ) };

		if ( !raw ) {
			return std::unexpected(
				fail( errc::json, write_error.msg != nullptr ? write_error.msg : "write error" ) );
		}

		return std::string{ raw.get( ) };
	}

	auto canonicalize( const std::string_view text ) -> result< std::string > {
		yyjson_read_err read_error{ };
		yyjson_doc* raw = yyjson_read_opts( const_cast< char* >( text.data( ) ), text.size( ), YYJSON_READ_NOFLAG,
			nullptr, &read_error );

		if ( raw == nullptr ) {
			auto message = std::string{ "yyjson: " };
			message += ( read_error.msg != nullptr ) ? read_error.msg : "parse error";

			return std::unexpected( fail( errc::json, std::move( message ) ) );
		}

		auto owned = std::unique_ptr< yyjson_doc, void ( * )( yyjson_doc* ) >(
			raw, yyjson_doc_free );

		auto converted = from_val( yyjson_doc_get_root( owned.get( ) ) );

		if ( !converted ) {
			return std::unexpected( converted.error( ) );
		}

		auto sorted = document::make_object( );

		if ( auto attached = sorted.set_node( "value", std::move( *converted ) ); !attached ) {
			return std::unexpected( attached.error( ) );
		}

		auto dumped = sorted.dump( );

		if ( !dumped ) {
			return std::unexpected( dumped.error( ) );
		}

		// Strip the {"value":...} wrapper the set_node key requires.
		const auto prefix = std::string_view{ R"({"value":)" };

		if ( dumped->size( ) < prefix.size( ) + 1 || dumped->substr( 0, prefix.size( ) ) != prefix ) {
			return std::unexpected( fail( errc::json, "canonicalize produced an unexpected shape" ) );
		}

		return dumped->substr( prefix.size( ), dumped->size( ) - prefix.size( ) - 1 );
	}

}
