#include "mcode/support/json.hxx"

#include "mcode/support/json_internal.hxx"

#include <yyjson.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace mcode::json {

	namespace {

		// One conversion for both accessors. yyjson_is_num admits real numbers, and
		// yyjson_get_sint returns 0 for anything that is not an integer, so a
		// gateway that sends a token count as 1530.0 produced zero tokens with no
		// diagnostic. yyjson_is_int admits uint as well, and get_sint reinterprets a
		// uint above INT64_MAX as negative.
		auto to_int64( yyjson_val* value, const std::string_view what ) -> result< std::int64_t > {
			if ( value == nullptr ) {
				return std::unexpected( fail( errc::json, "missing " + std::string{ what } ) );
			}

			if ( yyjson_is_sint( value ) ) {
				return yyjson_get_sint( value );
			}

			if ( yyjson_is_uint( value ) ) {
				const auto wide = yyjson_get_uint( value );

				if ( wide > static_cast< std::uint64_t >( std::numeric_limits< std::int64_t >::max( ) ) ) {
					return std::unexpected( fail( errc::json, std::string{ what } +
						" is too large for a 64-bit signed integer" ) );
				}

				return static_cast< std::int64_t >( wide );
			}

			return std::unexpected( fail( errc::json,
				std::string{ what } + " is not an integer" ) );
		}

	}

	auto node::make_boolean( const bool value ) -> node {
		auto out = node{ };
		out.type = kind::boolean;
		out.boolean = value;

		return out;
	}

	auto node::make_integer( const std::int64_t value ) -> node {
		auto out = node{ };
		out.type = kind::integer;
		out.integer = value;

		return out;
	}

	auto node::make_real( const double value ) -> node {
		auto out = node{ };
		out.type = kind::real;
		out.real = value;

		return out;
	}

	auto node::make_string( const std::string_view value ) -> node {
		auto out = node{ };
		out.type = kind::string;
		out.text = std::string{ value };

		return out;
	}

	auto node::make_array( ) -> node {
		auto out = node{ };
		out.type = kind::array;

		return out;
	}

	auto node::make_object( ) -> node {
		auto out = node{ };
		out.type = kind::object;

		return out;
	}

	auto node::member( const std::string_view key ) -> node* {
		const auto found = members.find( key );

		return found == members.end( ) ? nullptr : &found->second;
	}

	auto node::member( const std::string_view key ) const -> const node* {
		const auto found = members.find( key );

		return found == members.end( ) ? nullptr : &found->second;
	}

	document::document( mut_doc_pointer doc ) : mut_( std::move( doc ) ), mutable_( true ) {
		root_.type = node::kind::object;
	}

	document::~document( ) {
		if ( doc_ != nullptr ) {
			yyjson_doc_free( doc_ );
		}
	}

	document::document( document&& other ) noexcept
		: doc_( other.doc_ )
		, mut_( std::move( other.mut_ ) )
		, root_( std::move( other.root_ ) )
		, mutable_( other.mutable_ ) {
		other.doc_ = nullptr;
		other.mutable_ = false;
	}

	auto document::operator=( document&& other ) noexcept -> document& {
		if ( this != &other ) {
			if ( doc_ != nullptr ) {
				yyjson_doc_free( doc_ );
			}

			doc_ = other.doc_;
			mut_ = std::move( other.mut_ );
			root_ = std::move( other.root_ );
			mutable_ = other.mutable_;
			other.doc_ = nullptr;
			other.mutable_ = false;
		}

		return *this;
	}

	auto document::parse( const std::string_view text ) -> result< document > {
		if ( text.empty( ) ) {
			return std::unexpected( fail( errc::json, "empty input" ) );
		}

		auto read_error = yyjson_read_err{ };
		auto* doc = yyjson_read_opts( const_cast< char* >( text.data( ) ), text.size( ),
			YYJSON_READ_NOFLAG, nullptr, &read_error );

		if ( doc == nullptr ) {
			auto message = std::string{ "yyjson: " };
			message += ( read_error.msg != nullptr ) ? read_error.msg : "parse error";
			message += " at byte ";
			message += std::to_string( read_error.pos );

			return std::unexpected( fail( errc::json, std::move( message ) ) );
		}

		return document{ doc };
	}

	auto document::make_object( ) -> document {
		auto doc = mut_doc_pointer{ yyjson_mut_doc_new( nullptr ), detail::free_mut_doc };

		return document{ std::move( doc ) };
	}

	auto document::get_int( const std::string_view key ) const -> result< std::int64_t > {
		if ( mutable_ ) {
			const auto* entry = root_.member( key );

			if ( entry == nullptr ) {
				return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
			}

			if ( entry->type != node::kind::integer ) {
				return std::unexpected( fail( errc::json, "key is not an integer: " + std::string{ key } ) );
			}

			return entry->integer;
		}

		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "get_int on an empty document" ) );
		}

		auto* root = yyjson_doc_get_root( doc_ );

		if ( root == nullptr || !yyjson_is_obj( root ) ) {
			return std::unexpected( fail( errc::json, "root is not an object" ) );
		}

		auto* found = yyjson_obj_getn( root, key.data( ), key.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
		}

		return to_int64( found, "key " + std::string{ key } );
	}

	auto document::get_string( const std::string_view key ) const -> result< std::string > {
		if ( mutable_ ) {
			const auto* entry = root_.member( key );

			if ( entry == nullptr ) {
				return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
			}

			if ( entry->type != node::kind::string ) {
				return std::unexpected( fail( errc::json, "key is not a string: " + std::string{ key } ) );
			}

			return entry->text;
		}

		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "get_string on an empty document" ) );
		}

		auto* root = yyjson_doc_get_root( doc_ );

		if ( root == nullptr || !yyjson_is_obj( root ) ) {
			return std::unexpected( fail( errc::json, "root is not an object" ) );
		}

		auto* found = yyjson_obj_getn( root, key.data( ), key.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
		}

		if ( !yyjson_is_str( found ) ) {
			return std::unexpected( fail( errc::json, "key is not a string: " + std::string{ key } ) );
		}

		return std::string{ yyjson_get_str( found ), yyjson_get_len( found ) };
	}

	auto document::pointer( const std::string_view path ) const -> result< std::string > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* root = yyjson_doc_get_root( doc_ );
		auto* found = yyjson_ptr_getn( root, path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		if ( yyjson_is_str( found ) ) {
			return std::string{ yyjson_get_str( found ), yyjson_get_len( found ) };
		}

		const auto rendered = detail::owned_cstr{ yyjson_val_write( found, YYJSON_WRITE_NOFLAG, nullptr ) };

		if ( !rendered ) {
			return std::unexpected( fail( errc::json, "failed to render pointer value" ) );
		}

		return std::string{ rendered.get( ) };
	}

	auto document::pointer_string( const std::string_view path ) const -> result< std::string > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* found = yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		if ( !yyjson_is_str( found ) ) {
			return std::unexpected( fail( errc::json,
				"pointer is not a string: " + std::string{ path } ) );
		}

		return std::string{ yyjson_get_str( found ), yyjson_get_len( found ) };
	}

	namespace {

		auto object_keys_of( yyjson_val* object ) -> std::vector< std::string > {
			auto out = std::vector< std::string >{ };

			if ( object == nullptr || !yyjson_is_obj( object ) ) {
				return out;
			}

			auto iterator = yyjson_obj_iter{ };

			if ( !yyjson_obj_iter_init( object, &iterator ) ) {
				return out;
			}

			// yyjson's object iterator yields the KEY; the value is looked up from it.
			while ( auto* name = yyjson_obj_iter_next( &iterator ) ) {
				if ( yyjson_is_str( name ) ) {
					out.emplace_back( yyjson_get_str( name ), yyjson_get_len( name ) );
				}
			}

			return out;
		}

	}

	auto append_escaped( std::string& out, const std::string_view text ) -> void {
		for ( const auto character : text ) {
			switch ( character ) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;

				default:
					if ( static_cast< unsigned char >( character ) < 0x20 ) {
						auto buffer = std::array< char, 8 >{ };
						std::snprintf( buffer.data( ), buffer.size( ), "\\u%04x",
							static_cast< unsigned char >( character ) );
						out += buffer.data( );
					} else {
						out += character;
					}
			}
		}
	}

	auto document::keys_at( const std::string_view path ) const -> std::vector< std::string > {
		if ( doc_ == nullptr ) {
			return { };
		}

		return object_keys_of( yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) ) );
	}

	auto document::pointer_raw( const std::string_view path ) const -> result< std::string > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* found = yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		const auto rendered = detail::owned_cstr{ yyjson_val_write( found, YYJSON_WRITE_NOFLAG, nullptr ) };

		if ( !rendered ) {
			return std::unexpected( fail( errc::json, "failed to render pointer value" ) );
		}

		return std::string{ rendered.get( ) };
	}

	auto document::pointer_int( const std::string_view path ) const -> result< std::int64_t > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* found = yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		return to_int64( found, "pointer " + std::string{ path } );
	}

	auto document::pointer_bool( const std::string_view path ) const -> result< bool > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* found = yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		if ( !yyjson_is_bool( found ) ) {
			return std::unexpected( fail( errc::json,
				"pointer is not a boolean: " + std::string{ path } ) );
		}

		return yyjson_get_bool( found );
	}

	auto document::pointer_string_array( const std::string_view path ) const
		-> result< std::vector< std::string > > {
		if ( doc_ == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer on a non-parse document" ) );
		}

		auto* found = yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) );

		if ( found == nullptr ) {
			return std::unexpected( fail( errc::json, "pointer not found: " + std::string{ path } ) );
		}

		if ( !yyjson_is_arr( found ) ) {
			return std::unexpected( fail( errc::json,
				"pointer is not an array: " + std::string{ path } ) );
		}

		auto out = std::vector< std::string >{ };
		auto index = std::size_t{ 0 };
		auto count = std::size_t{ 0 };
		yyjson_val* item = nullptr;

		yyjson_arr_foreach( found, index, count, item ) {
			if ( !yyjson_is_str( item ) ) {
				return std::unexpected( fail( errc::json,
					"array element " + std::to_string( index ) + " is not a string: " +
					std::string{ path } ) );
			}

			out.emplace_back( yyjson_get_str( item ), yyjson_get_len( item ) );
		}

		return out;
	}

	auto document::has_pointer( const std::string_view path ) const noexcept -> bool {
		if ( doc_ == nullptr ) {
			return false;
		}

		return yyjson_ptr_getn( yyjson_doc_get_root( doc_ ), path.data( ), path.size( ) ) != nullptr;
	}

}
