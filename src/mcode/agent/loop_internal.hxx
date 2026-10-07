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

	// A response cut off by the output limit is a distinct failure from a malformed one: the
	// arguments are a valid prefix of a call that was never finished, so no repair can recover
	// them. Matched case-insensitively against the provider's own finish reason.
	inline constexpr std::string_view TRUNCATION_STOP_REASONS[] = {
		"length",
		"max_tokens",
		"max_output_tokens",
		"length_exceeded",
		"model_length",
	};

	[[nodiscard]] inline auto is_truncation_stop_reason( const std::string_view reason ) noexcept
		-> bool {
		for ( const auto marker : TRUNCATION_STOP_REASONS ) {
			if ( reason.size( ) != marker.size( ) ) {
				continue;
			}

			auto equal = true;

			for ( auto index = std::size_t{ 0 }; index < reason.size( ); ++index ) {
				const auto byte = static_cast< unsigned char >( reason[ index ] );
				const auto lowered = static_cast< char >(
					( byte >= 'A' && byte <= 'Z' ) ? byte + ( 'a' - 'A' ) : byte );

				if ( lowered != marker[ index ] ) {
					equal = false;

					break;
				}
			}

			if ( equal ) {
				return true;
			}
		}

		return false;
	}

	// the finish reason the provider reported for the turn, empty when it reported none.
	[[nodiscard]] inline auto turn_stop_reason( const std::vector< model::chat_event >& events )
		-> std::string {
		for ( auto index = events.rbegin( ); index != events.rend( ); ++index ) {
			if ( index->type == model::chat_event::kind::turn_done ) {
				return index->stop_reason;
			}
		}

		return { };
	}

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

	// The id is required: without it the model re-issues the call instead of reading the result.
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
					// The applier emits the accumulated name and id on every fragment, so the
					// last write for an index is the complete value; registering the index even
					// when it is still empty keeps an incomplete call visible to the loop.
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
