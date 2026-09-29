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

	// The call id is required, not optional: a tool message the model cannot
	// correlate with the call it answers is a result it cannot use. Rendered with
	// an empty id, the model reports the result as missing and re-issues the call.
	inline auto result_block( const tool_outcome& outcome, const std::string_view tool_call_id )
		-> model::block {
		auto block = model::block{ };
		block.kind = model::block_kind::tool_result;
		block.tool_call_id = std::string{ tool_call_id };
		block.is_error = !outcome.ok;
		block.result_json = outcome.ok ? outcome.content : outcome.error_message;

		return block;
	}

	inline auto collect_calls( const std::vector< model::chat_event >& events )
		-> std::vector< tool_call > {
		auto calls = std::vector< tool_call >{ };
		auto args = std::map< int, std::string >{ };
		auto names = std::map< int, std::string >{ };
		auto ids = std::map< int, std::string >{ };

		for ( const auto& event : events ) {
			switch ( event.type ) {
				case model::chat_event::kind::tool_call_delta: {
					// Mid-stream events carry one fragment; the applier's finish()
					// re-emits the whole call as one event. A fragment that extends
					// what is already accumulated is that snapshot, so it replaces;
					// anything else concatenates.
					names[ event.index ] = event.tool_name;
					ids[ event.index ] = event.tool_call_id;

					auto& accumulated = args[ event.index ];

					if ( !event.args_fragment.empty( ) ) {
						if ( event.args_fragment.starts_with( accumulated ) ) {
							accumulated = event.args_fragment;
						} else {
							accumulated += event.args_fragment;
						}
					}

					break;
				}

				default: break;
			}
		}

		for ( const auto& [ index, name ] : names ) {
			auto call = tool_call{ };
			call.id = ids.contains( index ) ? ids.at( index ) : std::string{ };
			call.name = name;
			call.args_json = args.contains( index ) ? args.at( index ) : "{}";

			calls.push_back( std::move( call ) );
		}

		return calls;
	}

} // namespace mcode::loop_internal
