#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/support/json.hxx"

namespace mcode::loop_internal {

	inline constexpr std::string_view NEAR_BUDGET_NOTE =
		"budget nearly exhausted - wrap up or report blockers";

	inline auto token_estimate( const std::string_view text ) noexcept -> std::int64_t {
		return static_cast< std::int64_t >( text.size( ) / CHARS_PER_TOKEN_ESTIMATE );
	}

	inline auto message_tokens( const model::message& value ) -> std::int64_t {
		auto total = std::int64_t{ 0 };

		for ( const auto& block : value.blocks ) {
			total += token_estimate( block.text );
			total += token_estimate( block.args_json );
			total += token_estimate( block.result_json );
		}

		return total;
	}

	inline auto history_tokens( const std::vector< model::message >& history ) -> std::int64_t {
		auto total = std::int64_t{ 0 };

		for ( const auto& value : history ) {
			total += message_tokens( value );
		}

		return total;
	}

	inline auto thrash_hash( const std::string_view tool_name, const std::string_view args_json )
		-> result< std::string > {
		auto canonical = args_json.empty( ) ? std::string{ "{}" }
											: json::canonicalize( args_json );

		if ( !canonical ) {
			return std::unexpected( canonical.error( ) );
		}

		auto combined = std::string{ tool_name };
		combined += '\x1f';
		combined += *canonical;

		return combined;
	}

	inline auto result_block( const tool_outcome& outcome ) -> model::block {
		auto block = model::block{ };
		block.kind = model::block_kind::tool_result;
		block.is_error = !outcome.ok;
		block.result_json = outcome.ok ? outcome.content : outcome.error_message;

		return block;
	}

	inline auto collect_calls( const std::vector< model::chat_event >& events )
		-> std::vector< tool_call > {
		auto calls = std::vector< tool_call >{ };
		auto args = std::map< int, std::string >{ };
		auto names = std::map< int, std::string >{ };

		for ( const auto& event : events ) {
			switch ( event.type ) {
				case model::chat_event::kind::tool_call_delta: {
					names[ event.index ] = event.tool_name;
					args[ event.index ] += event.args_fragment;

					break;
				}

				default: break;
			}
		}

		for ( const auto& [ index, name ] : names ) {
			calls.push_back( { name, args.contains( index ) ? args.at( index ) : "{}" } );
		}

		return calls;
	}

} // namespace mcode::loop_internal
