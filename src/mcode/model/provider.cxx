#include "mcode/model/provider.hxx"

#include <array>
#include <map>

#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

		auto is_json_pointer( const std::string_view text ) -> bool {
			// A JSON pointer is empty (whole document) or starts with '/'.
			return text.empty( ) || text.front( ) == '/';
		}

		auto is_absolute_url( const std::string_view text ) -> bool {
			return text.starts_with( "http://" ) || text.starts_with( "https://" );
		}

		// Returns void, not a status: an absent member is not a failure, and a
		// fallible signature on an infallible operation forces every caller either
		// to check a value that is always empty or to discard it.
		auto string_member( const json::document& doc, const std::string_view key,
			std::string& target ) -> void {
			if ( const auto found = doc.get_string( key ) ) {
				target = *found;
			}
		}

	}

	auto validate( const provider_descriptor& descriptor ) -> status {
		if ( descriptor.name.empty( ) ) {
			return std::unexpected( fail( errc::config, "provider has no name" ) );
		}

		if ( descriptor.endpoint.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"provider '" + descriptor.name + "' has no endpoint" ) );
		}

		if ( !is_absolute_url( descriptor.endpoint ) ) {
			// An unvalidated endpoint would let a descriptor point at file:// or a
			// bare host, which the egress policy cannot reason about.
			return std::unexpected( fail( errc::config,
				"provider '" + descriptor.name + "' endpoint must be an absolute http(s) URL" ) );
		}

		// An escape hatch needs no mapping; everything else needs at least one.
		if ( !descriptor.escape_hatch && descriptor.stream.text_delta.empty( ) &&
			descriptor.stream.tool_call_args.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"provider '" + descriptor.name + "' maps neither text nor tool-call deltas" ) );
		}

		const auto pointers = std::array< std::pair< const char*, const std::string* >, 11 >{ {
			{ "stream.text_delta", &descriptor.stream.text_delta },
			{ "stream.thinking_delta", &descriptor.stream.thinking_delta },
			{ "stream.tool_call_index", &descriptor.stream.tool_call_index },
			{ "stream.tool_call_id", &descriptor.stream.tool_call_id },
			{ "stream.tool_call_name", &descriptor.stream.tool_call_name },
			{ "stream.tool_call_args", &descriptor.stream.tool_call_args },
			{ "stream.finish_reason", &descriptor.stream.finish_reason },
			{ "stream.usage_input", &descriptor.stream.usage_input },
			{ "stream.usage_output", &descriptor.stream.usage_output },
			{ "stream.usage_cached_read", &descriptor.stream.usage_cached_read },
			{ "stream.usage_cache_write", &descriptor.stream.usage_cache_write },
		} };

		for ( const auto& [ field, value ] : pointers ) {
			if ( !is_json_pointer( *value ) ) {
				return std::unexpected( fail( errc::config,
					"provider '" + descriptor.name + "' field " + field +
					" is not a JSON pointer: " + *value ) );
			}
		}

		if ( descriptor.auth.from != auth_spec::source::none ) {
			if ( descriptor.auth.name.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"provider '" + descriptor.name + "' declares an auth source but no name" ) );
			}

			if ( descriptor.auth.header.empty( ) ) {
				return std::unexpected( fail( errc::config,
					"provider '" + descriptor.name + "' declares auth but no header" ) );
			}
		}

		return { };
	}

	auto descriptor_from_json( const std::string_view json_text ) -> result< provider_descriptor > {
		auto parsed = json::document::parse( json_text );

		if ( !parsed ) {
			return std::unexpected( fail( errc::json,
				"provider descriptor is not valid JSON: " + parsed.error( ).msg ) );
		}

		auto descriptor = provider_descriptor{ };

		if ( auto name = parsed->get_string( "name" ) ) {
			descriptor.name = *name;
		}

		if ( auto endpoint = parsed->get_string( "endpoint" ) ) {
			descriptor.endpoint = *endpoint;
		}

		if ( auto headers = parsed->pointer( "/extra_headers" ) ) {
			descriptor.extra_headers_json = *headers;
		}

		if ( auto hatch = parsed->pointer_bool( "/on_event" ) ) {
			descriptor.escape_hatch = *hatch;
		}

		if ( auto header = parsed->pointer_string( "/auth/header" ) ) {
			descriptor.auth.header = *header;
		}

		if ( auto name = parsed->pointer_string( "/auth/from" ) ) {
			if ( *name == "env" ) {
				descriptor.auth.from = auth_spec::source::environment;
			} else if ( *name == "config" ) {
				descriptor.auth.from = auth_spec::source::config;
			} else {
				return std::unexpected( fail( errc::config,
					"unknown auth source '" + *name + "', expected 'env' or 'config'" ) );
			}
		}

		// An `auth` block that is present but names no source is a typo, not an
		// intentional "no auth": `auth = { header = "..." }` would otherwise
		// validate and send an unauthenticated request. Same rule as the manifest's
		// unknown-key rejection (`19`).
		if ( parsed->has_pointer( "/auth" ) && descriptor.auth.from == auth_spec::source::none ) {
			return std::unexpected( fail( errc::config,
				"provider '" + descriptor.name +
				"' has an auth block but no source; expected auth.from = \"env\" or \"config\"" ) );
		}

		if ( auto name = parsed->pointer_string( "/auth/name" ) ) {
			descriptor.auth.name = *name;
		}

		if ( auto scheme = parsed->pointer_string( "/auth/scheme" ) ) {
			descriptor.auth.scheme = *scheme;
		}

		string_member( *parsed, "model", descriptor.request.model );
		string_member( *parsed, "messages", descriptor.request.messages );
		string_member( *parsed, "tools", descriptor.request.tools );

		if ( auto field = parsed->pointer_string( "/request/max_output_tokens" ) ) {
			descriptor.request.max_output_tokens = *field;
		}

		if ( auto field = parsed->pointer_string( "/request/temperature" ) ) {
			descriptor.request.temperature = *field;
		}

		if ( auto field = parsed->pointer_string( "/request/response_schema" ) ) {
			descriptor.request.response_schema = *field;
		}

		// The Lua-facing form nests the stream mapping, matching docs/26.
		if ( auto text = parsed->pointer_string( "/stream/text_delta" ) ) {
			descriptor.stream.text_delta = *text;
		}

		if ( auto thinking = parsed->pointer_string( "/stream/thinking_delta" ) ) {
			descriptor.stream.thinking_delta = *thinking;
		}

		if ( auto index = parsed->pointer_string( "/stream/tool_calls/index" ) ) {
			descriptor.stream.tool_call_index = *index;
		}

		if ( auto id = parsed->pointer_string( "/stream/tool_calls/id" ) ) {
			descriptor.stream.tool_call_id = *id;
		}

		if ( auto name = parsed->pointer_string( "/stream/tool_calls/name" ) ) {
			descriptor.stream.tool_call_name = *name;
		}

		if ( auto args = parsed->pointer_string( "/stream/tool_calls/args" ) ) {
			descriptor.stream.tool_call_args = *args;
		}

		if ( auto finish = parsed->pointer_string( "/stream/finish" ) ) {
			descriptor.stream.finish_reason = *finish;
		}

		if ( auto usage = parsed->pointer_string( "/stream/usage/in" ) ) {
			descriptor.stream.usage_input = *usage;
		}

		if ( auto usage = parsed->pointer_string( "/stream/usage/out" ) ) {
			descriptor.stream.usage_output = *usage;
		}

		if ( auto usage = parsed->pointer_string( "/stream/usage/cached_read" ) ) {
			descriptor.stream.usage_cached_read = *usage;
		}

		if ( auto usage = parsed->pointer_string( "/stream/usage/cache_write" ) ) {
			descriptor.stream.usage_cache_write = *usage;
		}

		if ( auto usage = parsed->pointer_string( "/stream/usage/reasoning" ) ) {
			descriptor.stream.usage_reasoning = *usage;
		}

		// Terminal markers differ per provider -- [OI] Chat Completions sends the
		// [DONE] sentinel, Anthropic sends a message_stop event, Responses sends
		// response.completed -- so this is a descriptor field, not a hardcoded list.
		if ( parsed->has_pointer( "/stream/terminal_events" ) ) {
			auto terminal = parsed->pointer_string_array( "/stream/terminal_events" );

			if ( !terminal ) {
				return std::unexpected( terminal.error( ) );
			}

			descriptor.stream.terminal_events = *terminal;
		}

		if ( auto validated = validate( descriptor ); !validated ) {
			return std::unexpected( validated.error( ) );
		}

		return descriptor;
	}


	auto provider_registry::add( provider_descriptor descriptor ) -> status {
		if ( descriptor.name.empty( ) ) {
			return std::unexpected( fail( errc::config, "a provider needs a name" ) );
		}

		if ( entries_.contains( descriptor.name ) ) {
			return std::unexpected( fail( errc::config,
				"provider '" + descriptor.name + "' is already declared" ) );
		}

		if ( auto checked = validate( descriptor ); !checked ) {
			return checked;
		}

		auto record = entry{ };
		record.descriptor = std::move( descriptor );

		entries_.emplace( record.descriptor.name, std::move( record ) );

		return { };
	}

	auto provider_registry::find( const std::string_view name ) const
		-> const provider_descriptor* {
		const auto found = entries_.find( std::string{ name } );

		return found != entries_.end( ) ? &found->second.descriptor : nullptr;
	}

	auto provider_registry::all( ) const -> std::vector< const provider_descriptor* > {
		auto out = std::vector< const provider_descriptor* >{ };

		for ( const auto& [ name, record ] : entries_ ) {
			(void)name;

			out.push_back( &record.descriptor );
		}

		return out;
	}

	auto provider_registry::owned_by( const std::string_view owner ) const
		-> std::vector< const provider_descriptor* > {
		auto out = std::vector< const provider_descriptor* >{ };

		for ( const auto& [ name, record ] : entries_ ) {
			(void)name;

			if ( record.owner == owner ) {
				out.push_back( &record.descriptor );
			}
		}

		return out;
	}

	auto provider_registry::remove_owner( const std::string_view owner ) -> std::size_t {
		if ( owner.empty( ) ) {
			return 0;
		}

		return std::erase_if( entries_,
			[owner]( const auto& item ) { return item.second.owner == owner; } );
	}

}
