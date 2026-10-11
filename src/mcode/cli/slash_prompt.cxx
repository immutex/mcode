#include "mcode/cli/slash.hxx"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcode::cli {

	namespace {

		// Splits `/prompt <name> [json]` into the name and the argument object. The
		// rest of the line is taken verbatim as JSON: re-quoting it here would make
		// one argument shape impossible to express.
		[[nodiscard]] auto split_prompt_arguments( const std::string_view arguments )
			-> std::pair< std::string, std::string > {
			auto name = std::string{ };
			auto rest = std::string{ };

			auto index = std::size_t{ 0 };

			while ( index < arguments.size( ) &&
				arguments[ index ] != ' ' && arguments[ index ] != '\t' ) {
				name.push_back( arguments[ index ] );
				++index;
			}

			while ( index < arguments.size( ) &&
				( arguments[ index ] == ' ' || arguments[ index ] == '\t' ) ) {
				++index;
			}

			rest.assign( arguments.substr( index ) );

			return { name, rest };
		}

		// A required argument the caller did not supply would be filled with an empty
		// string by the server, which reads as the user having meant it.
		[[nodiscard]] auto missing_required( const mcode::mcp::server_prompt& prompt,
			const std::string_view json_arguments ) -> std::string {
			if ( json_arguments.empty( ) ) {
				for ( const auto& argument : prompt.arguments ) {
					if ( argument.required ) {
						return argument.name;
					}
				}

				return { };
			}

			for ( const auto& argument : prompt.arguments ) {
				if ( !argument.required ) {
					continue;
				}

				if ( json_arguments.find( "\"" + argument.name + "\"" )
					== std::string_view::npos ) {
					return argument.name;
				}
			}

			return { };
		}

	}

	auto prompt_text( const mcode::mcp::server_set* servers,
		const std::string_view arguments, std::optional< std::string >& submit )
		-> std::string {
		if ( servers == nullptr || servers->all( ).empty( ) ) {
			return "no MCP servers are connected in this session";
		}

		const auto [wanted, json_arguments] = split_prompt_arguments( arguments );

		auto lines = std::string{ };
		auto found = false;

		for ( const auto& board : servers->all( ) ) {
			auto* client = board->client_ptr( );

			if ( client == nullptr || !client->is_alive( ) ) {
				continue;
			}

			const auto listed = client->list_prompts( );

			if ( !listed ) {
				lines += board->config( ).name + ": " + listed.error( ).msg + '\n';

				continue;
			}

			for ( const auto& prompt : *listed ) {
				if ( !wanted.empty( ) && prompt.name != wanted ) {
					continue;
				}

				found = true;

				if ( wanted.empty( ) ) {
					lines += board->config( ).name + "  " + prompt.name;

					if ( !prompt.description.empty( ) ) {
						lines += "  " + prompt.description;
					}

					lines += '\n';

					continue;
				}

				if ( const auto missing = missing_required( prompt, json_arguments );
					!missing.empty( ) ) {
					return "prompt '" + prompt.name + "' needs '" + missing + "': /prompt "
						+ prompt.name + " {\"" + missing + "\":\"...\"}";
				}

				const auto filled = client->get_prompt( prompt.name, json_arguments );

				if ( !filled ) {
					return filled.error( ).msg;
				}

				submit = filled->text;

				return { };
			}
		}

		if ( !found ) {
			return wanted.empty( ) ? "no connected server offers a prompt"
				: "no prompt named '" + wanted + "'";
		}

		return lines;
	}

}
