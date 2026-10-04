#include "mcode/ext/lua_json.hxx"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

#include "mcode/support/json.hxx"

namespace mcode::ext {

	namespace {

		// a payload nested past this would blow the C stack.
		constexpr auto MAX_JSON_DEPTH = 64;

		inline constexpr auto NUMBER_FORMAT = "%.17g";
		inline constexpr auto NUMBER_BUFFER_SIZE = std::size_t{ 32 };

		auto push_json_value( lua_State* state, yyjson_val* value, int depth ) -> status;

		auto push_json_object( lua_State* state, yyjson_val* object, int depth ) -> status {
			lua_createtable( state, 0, static_cast< int >( yyjson_obj_size( object ) ) );

			auto iterator = yyjson_obj_iter_with( object );
			yyjson_val* key = nullptr;

			while ( ( key = yyjson_obj_iter_next( &iterator ) ) != nullptr ) {
				if ( auto pushed = push_json_value( state, yyjson_obj_iter_get_val( key ),
					depth + 1 ); !pushed ) {
					lua_pop( state, 1 );

					return pushed;
				}

				// a JSON null becomes nil, which removes the key: the member is absent, not nil.
				lua_setfield( state, -2, yyjson_get_str( key ) );
			}

			return { };
		}

		auto push_json_array( lua_State* state, yyjson_val* array, int depth ) -> status {
			lua_createtable( state, static_cast< int >( yyjson_arr_size( array ) ), 0 );

			auto index = 1;
			auto iterator = yyjson_arr_iter_with( array );
			yyjson_val* element = nullptr;

			while ( ( element = yyjson_arr_iter_next( &iterator ) ) != nullptr ) {
				if ( auto pushed = push_json_value( state, element, depth + 1 ); !pushed ) {
					lua_pop( state, 1 );

					return pushed;
				}

				lua_rawseti( state, -2, index );
				++index;
			}

			return { };
		}

		auto push_json_value( lua_State* state, yyjson_val* value, int depth ) -> status {
			if ( depth > MAX_JSON_DEPTH ) {
				return std::unexpected( fail( errc::config, "JSON nesting exceeds 64 levels" ) );
			}

			if ( yyjson_is_null( value ) ) {
				lua_pushnil( state );

				return { };
			}

			if ( yyjson_is_bool( value ) ) {
				lua_pushboolean( state, yyjson_get_bool( value ) ? 1 : 0 );

				return { };
			}

			if ( yyjson_is_num( value ) ) {
				lua_pushnumber( state, yyjson_get_num( value ) );

				return { };
			}

			if ( yyjson_is_str( value ) ) {
				lua_pushlstring( state, yyjson_get_str( value ),
					static_cast< std::size_t >( yyjson_get_len( value ) ) );

				return { };
			}

			if ( yyjson_is_obj( value ) ) {
				return push_json_object( state, value, depth );
			}

			if ( yyjson_is_arr( value ) ) {
				return push_json_array( state, value, depth );
			}

			return std::unexpected( fail( errc::config, "unsupported JSON value" ) );
		}

	}

	namespace {

		struct encode_context {
			std::set< const void* > active;
			int depth = 0;
		};

		auto encode_value( lua_State* state, int index, std::string& out,
			encode_context& context ) -> status;

		// the key is kept, not its rendered text: a numeric key is not a field name.
		struct table_key {
			bool numeric = false;
			double number = 0.0;
			std::string text;
		};

		auto encode_table( lua_State* state, const int index, std::string& out,
			encode_context& context ) -> status {
			const auto* identity = lua_topointer( state, index );

			if ( context.active.contains( identity ) ) {
				return std::unexpected( fail( errc::config,
					"a cyclic table cannot be encoded as JSON" ) );
			}

			if ( context.depth > MAX_JSON_DEPTH ) {
				return std::unexpected( fail( errc::config, "table nesting exceeds 64 levels" ) );
			}

			context.active.insert( identity );
			++context.depth;

			const auto table = lua_absindex( state, index );
			const auto length = static_cast< std::size_t >( lua_objlen( state, table ) );

			auto keys = std::vector< table_key >{ };
			auto array_like = true;
			auto count = std::size_t{ 0 };

			lua_pushnil( state );

			while ( lua_next( state, table ) != 0 ) {
				++count;

				auto key = table_key{ };
				const auto key_type = lua_type( state, -2 );

				if ( key_type == LUA_TNUMBER ) {
					key.numeric = true;
					key.number = lua_tonumber( state, -2 );

					if ( key.number < 1.0 || key.number > static_cast< double >( length ) ||
						key.number != std::floor( key.number ) ) {
						array_like = false;
					}

					auto buffer = std::array< char, NUMBER_BUFFER_SIZE >{ };
					std::snprintf( buffer.data( ), buffer.size( ), NUMBER_FORMAT, key.number );
					key.text.assign( buffer.data( ) );
				} else if ( key_type == LUA_TSTRING ) {
					auto key_length = std::size_t{ 0 };
					const auto* text = lua_tolstring( state, -2, &key_length );

					key.text.assign( text != nullptr ? text : "", key_length );

					array_like = false;
				} else {
					// both the key and its value: the iteration never resumes from here.
					lua_pop( state, 2 );

					context.active.erase( identity );
					--context.depth;

					return std::unexpected( fail( errc::config,
						"a table key must be a string or a number" ) );
				}

				keys.push_back( std::move( key ) );

				lua_pop( state, 1 );
			}

			if ( array_like && count == length ) {
				out += '[';

				for ( auto element = std::size_t{ 1 }; element <= length; ++element ) {
					if ( element > std::size_t{ 1 } ) {
						out += ',';
					}

					lua_rawgeti( state, table, static_cast< int >( element ) );

					if ( auto encoded = encode_value( state, -1, out, context ); !encoded ) {
						lua_pop( state, 1 );

						context.active.erase( identity );
						--context.depth;

						return encoded;
					}

					lua_pop( state, 1 );
				}

				out += ']';

				context.active.erase( identity );
				--context.depth;

				return { };
			}

			// sorted so two runs emit byte-identical JSON, which the prompt cache keys on.
			std::sort( keys.begin( ), keys.end( ),
				[]( const table_key& left, const table_key& right ) {
					return left.text < right.text;
				} );

			out += '{';

			auto first = true;

			for ( const auto& key : keys ) {
				if ( !first ) {
					out += ',';
				}

				first = false;

				out += '"';
				json::append_escaped( out, key.text );
				out += "\":";

				if ( key.numeric ) {
					lua_pushnumber( state, key.number );
				} else {
					lua_pushlstring( state, key.text.data( ), key.text.size( ) );
				}

				lua_rawget( state, table );

				if ( auto encoded = encode_value( state, -1, out, context ); !encoded ) {
					lua_pop( state, 1 );

					context.active.erase( identity );
					--context.depth;

					return encoded;
				}

				lua_pop( state, 1 );
			}

			out += '}';

			context.active.erase( identity );
			--context.depth;

			return { };
		}

		auto encode_value( lua_State* state, const int index, std::string& out,
			encode_context& context ) -> status {
			switch ( lua_type( state, index ) ) {
				case LUA_TNIL:
					out += "null";

					return { };

				case LUA_TBOOLEAN:
					out += lua_toboolean( state, index ) != 0 ? "true" : "false";

					return { };

				case LUA_TNUMBER: {
					const auto number = lua_tonumber( state, index );

					if ( !std::isfinite( number ) ) {
						return std::unexpected( fail( errc::config,
							"a non-finite number cannot be encoded as JSON" ) );
					}

					auto buffer = std::array< char, NUMBER_BUFFER_SIZE >{ };
					std::snprintf( buffer.data( ), buffer.size( ), NUMBER_FORMAT, number );
					out += buffer.data( );

					return { };
				}

				case LUA_TSTRING: {
					auto length = std::size_t{ 0 };
					const auto* text = lua_tolstring( state, index, &length );

					out += '"';
					json::append_escaped( out, std::string_view{ text, length } );
					out += '"';

					return { };
				}

				case LUA_TTABLE:
					return encode_table( state, index, out, context );

				default:
					return std::unexpected( fail( errc::config,
						std::string{ "a " } + lua_typename( state, lua_type( state, index ) ) +
						" cannot be encoded as JSON" ) );
			}
		}

	}

	auto json_from_lua( lua_State* state, const int index ) -> result< std::string > {
		auto out = std::string{ };
		auto context = encode_context{ };

		if ( auto encoded = encode_value( state, index, out, context ); !encoded ) {
			return std::unexpected( encoded.error( ) );
		}

		return out;
	}

	auto push_json( lua_State* state, const std::string_view text ) -> status {
		auto error = yyjson_read_err{ };
		auto* document = yyjson_read_opts( const_cast< char* >( text.data( ) ), text.size( ),
			YYJSON_READ_ALLOW_INF_AND_NAN, nullptr, &error );

		if ( document == nullptr ) {
			return std::unexpected( fail( errc::config,
				"invalid JSON: " + std::string{ error.msg != nullptr ? error.msg : "parse error" } ) );
		}

		auto* root = yyjson_doc_get_root( document );

		auto pushed = push_json_value( state, root, 0 );

		yyjson_doc_free( document );

		return pushed;
	}

}
