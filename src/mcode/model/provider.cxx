#include "mcode/model/provider.hxx"

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <string>

#include "mcode/support/json.hxx"

namespace mcode::model {

	namespace {

		auto is_json_pointer( const std::string_view text ) -> bool {
			// Empty means the whole document. Otherwise a JSON pointer is a sequence
			// of `/`-prefixed tokens.
			if ( text.empty( ) ) {
				return true;
			}

			if ( text.front( ) != '/' ) {
				return false;
			}

			// The applier resolves pointers literally, so the two RFC 6901 forms it
			// cannot honour are refused here rather than resolving to nothing at
			// first token: `/-` means "append to the array" and `*` is not a
			// wildcard in a JSON pointer at all.
			if ( text.find( "/-" ) != std::string_view::npos ) {
				return false;
			}

			return text.find( '*' ) == std::string_view::npos;
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

		// Every pointer the descriptor can carry, so a typo is caught here rather
		// than silently dropping a field at first token. Adding a pointer to the
		// struct without adding it here is the bug this array exists to prevent.
		const auto pointers = std::array< std::pair< const char*, const std::string* >, 14 >{ {
			{ "stream.text_delta", &descriptor.stream.text_delta },
			{ "stream.thinking_delta", &descriptor.stream.thinking_delta },
			{ "stream.tool_call_index", &descriptor.stream.tool_call_index },
			{ "stream.tool_call_id", &descriptor.stream.tool_call_id },
			{ "stream.tool_call_name", &descriptor.stream.tool_call_name },
			{ "stream.tool_call_args", &descriptor.stream.tool_call_args },
			{ "stream.finish_reason", &descriptor.stream.finish_reason },
			{ "stream.error_message", &descriptor.stream.error_message },
			{ "stream.error_code", &descriptor.stream.error_code },
			{ "stream.usage_input", &descriptor.stream.usage_input },
			{ "stream.usage_output", &descriptor.stream.usage_output },
			{ "stream.usage_cached_read", &descriptor.stream.usage_cached_read },
			{ "stream.usage_cache_write", &descriptor.stream.usage_cache_write },
			{ "stream.usage_reasoning", &descriptor.stream.usage_reasoning },
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

	namespace {

		// The keys a descriptor accepts, per level. A typo such as `steam` for
		// `stream` would otherwise be dropped silently, and the descriptor would load
		// with the field unset -- the first-token failure validation exists to
		// prevent exactly that.
		inline constexpr auto ALLOWED_TOP_LEVEL = std::array< std::string_view, 7 >{
			"name", "endpoint", "auth", "request", "stream", "extra_headers", "on_event",
		};

		inline constexpr auto ALLOWED_AUTH = std::array< std::string_view, 4 >{
			"from", "name", "header", "scheme",
		};

		inline constexpr auto ALLOWED_STREAM = std::array< std::string_view, 9 >{
			"text_delta", "thinking_delta", "tool_calls", "finish", "usage", "error",
			"terminal_events", "text_events", "tool_call_events",
		};

		inline constexpr auto ALLOWED_ERROR = std::array< std::string_view, 2 >{
			"message", "code",
		};

		inline constexpr auto ALLOWED_TOOL_CALLS = std::array< std::string_view, 4 >{
			"index", "id", "name", "args",
		};

		inline constexpr auto ALLOWED_USAGE = std::array< std::string_view, 5 >{
			"in", "out", "cached_read", "cache_write", "reasoning",
		};

		inline constexpr auto ALLOWED_REQUEST = std::array< std::string_view, 7 >{
			"model", "messages", "tools", "max_output_tokens", "temperature",
			"response_schema", "reasoning_effort",
		};

		auto reject_unknown_at( const json::document& document, const std::string_view path,
			const std::span< const std::string_view > allowed ) -> status {
			for ( const auto& key : document.keys_at( path ) ) {
				if ( std::find( allowed.begin( ), allowed.end( ), key ) == allowed.end( ) ) {
					return std::unexpected( fail( errc::config,
						"unknown descriptor key '" + key + "' at '" +
						( path.empty( ) ? std::string{ "<root>" } : std::string{ path } ) + "'" ) );
				}
			}

			return { };
		}

		auto reject_unknown_keys( const json::document& document ) -> status {
			// Every level is checked, including the ones the descriptor may omit. A
			// block that is absent yields no keys, so this costs nothing when the
			// descriptor is minimal.
			const auto levels = std::array< std::pair< const char*, std::span< const std::string_view > >, 7 >{ {
				{ "", ALLOWED_TOP_LEVEL },
				{ "/auth", ALLOWED_AUTH },
				{ "/stream", ALLOWED_STREAM },
				{ "/stream/tool_calls", ALLOWED_TOOL_CALLS },
				{ "/stream/usage", ALLOWED_USAGE },
				{ "/stream/error", ALLOWED_ERROR },
				{ "/request", ALLOWED_REQUEST },
			} };

			for ( const auto& [path, allowed] : levels ) {
				if ( auto known = reject_unknown_at( document, path, allowed ); !known ) {
					return known;
				}
			}

			return { };
		}

	}

	auto descriptor_from_json( const std::string_view json_text ) -> result< provider_descriptor > {
		auto parsed = json::document::parse( json_text );

		if ( !parsed ) {
			return std::unexpected( fail( errc::json,
				"provider descriptor is not valid JSON: " + parsed.error( ).msg ) );
		}

		if ( auto known = reject_unknown_keys( *parsed ); !known ) {
			return std::unexpected( known.error( ) );
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

		// The Lua-facing form nests the stream mapping.
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

		if ( auto message = parsed->pointer_string( "/stream/error/message" ) ) {
			descriptor.stream.error_message = *message;
		}

		if ( auto code = parsed->pointer_string( "/stream/error/code" ) ) {
			descriptor.stream.error_code = *code;
		}

		if ( parsed->has_pointer( "/stream/text_events" ) ) {
			auto gated = parsed->pointer_string_array( "/stream/text_events" );

			if ( !gated ) {
				return std::unexpected( gated.error( ) );
			}

			descriptor.stream.text_events = *gated;
		}

		if ( parsed->has_pointer( "/stream/tool_call_events" ) ) {
			auto gated = parsed->pointer_string_array( "/stream/tool_call_events" );

			if ( !gated ) {
				return std::unexpected( gated.error( ) );
			}

			descriptor.stream.tool_call_events = *gated;
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


	auto provider_registry::add( provider_descriptor descriptor, std::string owner ) -> status {
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
		record.owner = std::move( owner );

		entries_.emplace( record.descriptor.name, std::move( record ) );

		return { };
	}

	auto provider_registry::find( const std::string_view name ) const
		-> const provider_descriptor* {
		// std::less<> makes the map transparent, so this does not build a string.
		const auto found = entries_.find( name );

		return found != entries_.end( ) ? &found->second.descriptor : nullptr;
	}

	auto provider_registry::all( ) const -> std::vector< const provider_descriptor* > {
		auto out = std::vector< const provider_descriptor* >{ };

		for ( const auto& [ name, record ] : entries_ ) {
			out.push_back( &record.descriptor );
		}

		return out;
	}

	auto provider_registry::owned_by( const std::string_view owner ) const
		-> std::vector< const provider_descriptor* > {
		auto out = std::vector< const provider_descriptor* >{ };

		for ( const auto& [ name, record ] : entries_ ) {
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
