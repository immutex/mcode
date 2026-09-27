#include "mcode/support/json.hxx"

#include <yyjson.h>

#include <cstdlib>
#include <memory>
#include <utility>

namespace mcode::json {

	namespace {

		struct free_deleter {
			auto operator( )( char* pointer ) const noexcept -> void { std::free( pointer ); }
		};

		using owned_cstr = std::unique_ptr< char, free_deleter >;

		auto free_mut_doc( yyjson_mut_doc* doc ) noexcept -> void {
			if ( doc != nullptr ) {
				yyjson_mut_doc_free( doc );
			}
		}

		[[nodiscard]] auto write_flags( const bool pretty ) -> yyjson_write_flag {
			return pretty ? YYJSON_WRITE_PRETTY : YYJSON_WRITE_NOFLAG;
		}

	}

	document::document( mut_doc_pointer doc ) : mut_( std::move( doc ) ), mutable_( true ) { }

	document::~document( ) {
		if ( doc_ != nullptr ) {
			yyjson_doc_free( doc_ );
		}
	}

	document::document( document&& other ) noexcept
		: doc_( other.doc_ )
		, mut_( std::move( other.mut_ ) )
		, members_( std::move( other.members_ ) )
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
			members_ = std::move( other.members_ );
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
		auto doc = mut_doc_pointer{ yyjson_mut_doc_new( nullptr ), free_mut_doc };

		return document{ std::move( doc ) };
	}

	auto document::get_int( const std::string_view key ) const -> result< std::int64_t > {
		if ( mutable_ ) {
			const auto entry = members_.find( key );

			if ( entry == members_.end( ) ) {
				return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
			}

			if ( entry->second.type != value::kind::integer ) {
				return std::unexpected( fail( errc::json, "key is not an integer: " + std::string{ key } ) );
			}

			return entry->second.number;
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

		if ( !yyjson_is_int( found ) ) {
			return std::unexpected( fail( errc::json, "key is not an integer: " + std::string{ key } ) );
		}

		return yyjson_get_sint( found );
	}

	auto document::get_string( const std::string_view key ) const -> result< std::string > {
		if ( mutable_ ) {
			const auto entry = members_.find( key );

			if ( entry == members_.end( ) ) {
				return std::unexpected( fail( errc::json, "missing key: " + std::string{ key } ) );
			}

			if ( entry->second.type != value::kind::string ) {
				return std::unexpected( fail( errc::json, "key is not a string: " + std::string{ key } ) );
			}

			return entry->second.text;
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

		const auto rendered = owned_cstr{ yyjson_val_write( found, YYJSON_WRITE_NOFLAG, nullptr ) };

		if ( !rendered ) {
			return std::unexpected( fail( errc::json, "failed to render pointer value" ) );
		}

		return std::string{ rendered.get( ) };
	}

	auto document::set_string( const std::string_view key, const std::string_view text ) -> status {
		if ( !mutable_ ) {
			return std::unexpected( fail( errc::json, "set_string on a non-mutable document" ) );
		}

		if ( key.empty( ) ) {
			return std::unexpected( fail( errc::json, "empty key" ) );
		}

		members_[ std::string{ key } ] = value{ .type = value::kind::string, .text = std::string{ text } };

		return { };
	}

	auto document::set_int( const std::string_view key, const std::int64_t number ) -> status {
		if ( !mutable_ ) {
			return std::unexpected( fail( errc::json, "set_int on a non-mutable document" ) );
		}

		if ( key.empty( ) ) {
			return std::unexpected( fail( errc::json, "empty key" ) );
		}

		members_[ std::string{ key } ] = value{ .type = value::kind::integer, .number = number };

		return { };
	}

	auto document::size( ) const noexcept -> std::size_t {
		if ( mutable_ ) {
			return members_.size( );
		}

		if ( doc_ == nullptr ) {
			return 0;
		}

		auto* root = yyjson_doc_get_root( doc_ );

		return ( root != nullptr && yyjson_is_obj( root ) ) ? yyjson_obj_size( root ) : 0;
	}

	auto document::dump( const bool pretty ) const -> result< std::string > {
		if ( mutable_ ) {
			auto doc = mut_doc_pointer{ yyjson_mut_doc_new( nullptr ), free_mut_doc };

			if ( !doc ) {
				return std::unexpected( fail( errc::json, "yyjson_mut_doc_new failed" ) );
			}

			auto* root = yyjson_mut_obj( doc.get( ) );

			if ( root == nullptr ) {
				return std::unexpected( fail( errc::json, "yyjson_mut_obj failed" ) );
			}

			yyjson_mut_doc_set_root( doc.get( ), root );

			for ( const auto& [ key, member ] : members_ ) {
				auto added = false;

				if ( member.type == value::kind::string ) {
					added = yyjson_mut_obj_add_strncpy( doc.get( ), root, key.c_str( ),
						member.text.data( ), member.text.size( ) );
				} else {
					added = yyjson_mut_obj_add_int( doc.get( ), root, key.c_str( ), member.number );
				}

				if ( !added ) {
					return std::unexpected( fail( errc::json, "failed to add member: " + key ) );
				}
			}

			auto write_error = yyjson_write_err{ };
			const auto raw = owned_cstr{
				yyjson_mut_write_opts( doc.get( ), write_flags( pretty ), nullptr, nullptr, &write_error ) };

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
			owned_cstr{ yyjson_write_opts( doc_, write_flags( pretty ), nullptr, nullptr, &write_error ) };

		if ( !raw ) {
			return std::unexpected(
				fail( errc::json, write_error.msg != nullptr ? write_error.msg : "write error" ) );
		}

		return std::string{ raw.get( ) };
	}

}
