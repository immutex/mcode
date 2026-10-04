#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace {

	auto read_line( std::string& line ) -> bool {
		line.clear( );

		int character = 0;

		while ( ( character = std::cin.get( ) ) != EOF ) {
			if ( character == '\n' ) {
				return true;
			}

			line.push_back( static_cast< char >( character ) );
		}

		return !line.empty( );
	}

	auto extract_id( const std::string& line ) -> std::uint64_t {
		const auto id_key = line.find( "\"id\"" );

		if ( id_key == std::string::npos ) {
			return 0;
		}

		const auto colon = line.find( ':', id_key );

		if ( colon == std::string::npos ) {
			return 0;
		}

		auto index = colon + 1;

		while ( index < line.size( ) && std::isspace( static_cast< unsigned char >( line[ index ] ) ) ) {
			++index;
		}

		auto value = std::uint64_t{ 0 };
		auto digits = 0;

		while ( index < line.size( ) && std::isdigit( static_cast< unsigned char >( line[ index ] ) ) ) {
			value = value * 10 + static_cast< std::uint64_t >( line[ index ] - '0' );
			++index;
			++digits;
		}

		return digits > 0 ? value : 0;
	}

	auto extract_method( const std::string& line ) -> std::string {
		const auto method_key = line.find( "\"method\"" );

		if ( method_key == std::string::npos ) {
			return { };
		}

		const auto quote_begin = line.find( '"', method_key + 8 );

		if ( quote_begin == std::string::npos ) {
			return { };
		}

		const auto quote_end = line.find( '"', quote_begin + 1 );

		if ( quote_end == std::string::npos ) {
			return { };
		}

		return line.substr( quote_begin + 1, quote_end - quote_begin - 1 );
	}

	auto respond( const std::uint64_t id, const std::string& result_json ) -> void {
		std::cout << R"({"jsonrpc":"2.0","id":)" << id << R"(,"result":)" << result_json
			<< "}\n" << std::flush;
	}

	auto respond_error( const std::uint64_t id, const std::string& message ) -> void {
		std::cout << R"({"jsonrpc":"2.0","id":)" << id << R"(,"error":{"code":-32603,)"
			<< R"("message":")" << message << R"("}})" << "\n" << std::flush;
	}

	auto tools_list_body( const bool variant ) -> std::string {
		if ( variant ) {
			return R"({"tools":[)"
				R"({"name":"upper","description":"CHANGED: uppercases text",)"
				R"("inputSchema":{"type":"object","properties":{"text":{"type":"string","changed":true}},"required":["text"]}},)"
				R"({"name":"count","description":"CHANGED: counts words",)"
				R"("inputSchema":{"type":"object","properties":{"text":{"type":"string"}}}})"
				"]}";
		}

		return R"({"tools":[)"
			R"({"name":"upper","description":"Uppercases the given text",)"
			R"("inputSchema":{"type":"object","properties":{"text":{"type":"string","description":"text to uppercase"}},"required":["text"]}},)"
			R"({"name":"count","description":"Counts characters of the given text",)"
			R"("inputSchema":{"type":"object","properties":{"text":{"type":"string","description":"text to count"}}}})"
			"]}";
	}

	auto extract_named_field( const std::string& line, const std::string_view key )
		-> std::string {
		const auto needle = "\"" + std::string{ key } + "\"";
		const auto key_at = line.find( needle );

		if ( key_at == std::string::npos ) {
			return { };
		}

		auto index = key_at + needle.size( );

		while ( index < line.size( ) && line[ index ] != ':' ) {
			++index;
		}

		++index;

		while ( index < line.size( ) && std::isspace( static_cast< unsigned char >( line[ index ] ) ) ) {
			++index;
		}

		if ( index >= line.size( ) || line[ index ] != '"' ) {
			return { };
		}

		++index;

		auto out = std::string{ };

		while ( index < line.size( ) && line[ index ] != '"' ) {
			if ( line[ index ] == '\\' && index + 1 < line.size( ) ) {
				++index;

				switch ( line[ index ] ) {
					case 'n': out.push_back( '\n' ); break;
					case 't': out.push_back( '\t' ); break;
					case 'r': out.push_back( '\r' ); break;
					default: out.push_back( line[ index ] ); break;
				}

				++index;

				continue;
			}

			out.push_back( line[ index ] );
			++index;
		}

		return out;
	}

	auto run_normal( ) -> int {
		auto list_calls = std::uint64_t{ 0 };

		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method.empty( ) ) {
				continue;
			}

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{"listChanged":false}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				++list_calls;
				respond( extract_id( line ), tools_list_body( list_calls == 2 ) );

				continue;
			}

			if ( method == "tools/call" ) {
				const auto text = extract_named_field( line, "text" );
				const auto name = extract_named_field( line, "name" );

				if ( name == "upper" ) {
					auto raised = std::string{ };

					for ( const auto character : text ) {
						raised.push_back( static_cast< char >( std::toupper(
							static_cast< unsigned char >( character ) ) ) );
					}

					respond( extract_id( line ),
						R"({"content":[{"type":"text","text":")" + raised + R"("}]})" );
				} else if ( name == "count" ) {
					respond( extract_id( line ),
						R"({"content":[{"type":"text","text":")" +
						std::to_string( text.size( ) ) + R"("}]})" );
				} else {
					respond_error( extract_id( line ), "unknown tool" );
				}

				continue;
			}

			respond_error( extract_id( line ), "unknown method" );
		}

		return 0;
	}

	auto run_banner( ) -> int {
		std::cout << "mcp-echo banner: this line is not JSON\n" << std::flush;
		std::cout << "another noise line\n" << std::flush;

		return run_normal( );
	}

	auto run_exit_mid_request( ) -> int {
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				respond( extract_id( line ), tools_list_body( false ) );

				continue;
			}

			// a request after the handshake is answered by exiting
			std::cerr << "exiting mid-request as instructed" << std::endl;

			return 42;
		}

		return 0;
	}

	auto run_hang( ) -> int {
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				respond( extract_id( line ), tools_list_body( false ) );

				continue;
			}

			continue;
		}

		return 0;
	}

	auto run_stderr_exit( ) -> int {
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				std::cerr << "trouble before the list" << std::endl;
				respond( extract_id( line ), tools_list_body( false ) );

				continue;
			}

			std::cerr << "fatal trouble on the call" << std::endl;

			return 3;
		}

		return 0;
	}

	auto run_changed_list( ) -> int {
		auto list_calls = std::uint64_t{ 0 };
		auto initialized = false;
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				++list_calls;
				respond( extract_id( line ), tools_list_body( list_calls == 2 ) );

				continue;
			}

			if ( method == "tools/call" && initialized ) {
				const auto name = extract_named_field( line, "name" );
				const auto text = extract_named_field( line, "text" );

				if ( name == "upper" ) {
					auto raised = std::string{ };

					for ( const auto character : text ) {
						raised.push_back( static_cast< char >( std::toupper(
							static_cast< unsigned char >( character ) ) ) );
					}

					respond( extract_id( line ),
						R"({"content":[{"type":"text","text":")" + raised + R"("}]})" );
				} else {
					respond_error( extract_id( line ), "unknown tool" );
				}

				continue;
			}

			respond_error( extract_id( line ), "unknown method" );
		}

		return 0;
	}

	auto run_late( ) -> int {
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				respond( extract_id( line ), tools_list_body( false ) );

				continue;
			}

			if ( method == "tools/call" ) {
				std::this_thread::sleep_for( std::chrono::seconds{ 2 } );

				const auto text = extract_named_field( line, "text" );
				auto raised = std::string{ };

				for ( const auto character : text ) {
					raised.push_back( static_cast< char >( std::toupper(
						static_cast< unsigned char >( character ) ) ) );
				}

				respond( extract_id( line ),
					R"({"content":[{"type":"text","text":")" + raised + R"("}]})" );

				continue;
			}

			respond_error( extract_id( line ), "unknown method" );
		}

		return 0;
	}

	auto run_echo_notify( ) -> int {
		auto line = std::string{ };

		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( line.find( "\"method\"" ) != std::string::npos &&
				line.find( "\"id\"" ) == std::string::npos ) {
				std::cout << line << "\n" << std::flush;

				continue;
			}

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "notifications/initialized" ) {
				continue;
			}

			if ( method == "tools/list" ) {
				respond( extract_id( line ), tools_list_body( false ) );

				continue;
			}

			continue;
		}

		return 0;
	}

	// answers initialize; returns the tools/list line, or empty on EOF
	auto read_until_tools_list( ) -> std::string {
		auto line = std::string{ };
		while ( read_line( line ) ) {
			const auto method = extract_method( line );

			if ( method == "initialize" ) {
				respond( extract_id( line ),
					R"({"protocolVersion":"2025-06-18",)"
					R"("capabilities":{"tools":{}},)"
					R"("serverInfo":{"name":"mcp-echo","version":"1.0.0"}})" );

				continue;
			}

			if ( method == "tools/list" ) {
				return line;
			}
		}

		return { };
	}

	// a request the client must answer; a string id exposes the request/notification confusion
	auto run_server_request( ) -> int {
		const auto list = read_until_tools_list( );
		if ( list.empty( ) ) {
			return 0;
		}

		respond( extract_id( list ), tools_list_body( false ) );
		std::cout << R"({"jsonrpc":"2.0","id":"srv-1","method":"roots/list"})" << "\n"
			<< std::flush;

		auto line = std::string{ };
		while ( read_line( line ) ) {
			if ( line.find( "\"id\":\"srv-1\"" ) == std::string::npos ) {
				continue;
			}

			const auto error_only = line.find( "\"error\"" ) != std::string::npos &&
				line.find( "\"result\"" ) == std::string::npos;

			std::cerr << ( error_only ? "reply-error" : "reply-result" ) << std::endl;
		}

		return 0;
	}

	// the response has no trailing newline, so only an EOF flush can deliver it
	auto run_unterminated( ) -> int {
		const auto list = read_until_tools_list( );
		if ( list.empty( ) ) {
			return 0;
		}

		std::cout << R"({"jsonrpc":"2.0","id":)" << extract_id( list )
			<< R"(,"result":)" << tools_list_body( false ) << "}" << std::flush;
		return 0;
	}

	// one unterminated line past the frame bound: the transport must fail, not grow
	auto run_oversized( ) -> int {
		const auto list = read_until_tools_list( );
		if ( list.empty( ) ) {
			return 0;
		}

		std::cout << std::string( 9u * 1024u * 1024u, 'x' ) << std::flush;
		return 0;
	}

}

auto main( const int argc, const char** argv ) -> int {
#ifdef _WIN32
	// binary mode is required: text mode translates \n to \r\n and breaks newline framing
	_setmode( _fileno( stdout ), _O_BINARY );
	_setmode( _fileno( stdin ), _O_BINARY );
#endif

	const auto mode = argc > 1 ? std::string_view{ argv[ 1 ] } : std::string_view{ "normal" };

	if ( mode == "banner" ) {
		return run_banner( );
	}

	if ( mode == "exit-mid-request" ) {
		return run_exit_mid_request( );
	}

	if ( mode == "hang" ) {
		return run_hang( );
	}

	if ( mode == "stderr-exit" ) {
		return run_stderr_exit( );
	}

	if ( mode == "changed-list" ) {
		return run_changed_list( );
	}

	if ( mode == "late" ) {
		return run_late( );
	}

	if ( mode == "echo-notify" ) {
		return run_echo_notify( );
	}

	if ( mode == "server-request" ) {
		return run_server_request( );
	}

	if ( mode == "unterminated" ) {
		return run_unterminated( );
	}

	if ( mode == "oversized" ) {
		return run_oversized( );
	}

	return run_normal( );
}
