#include "mcode/model/types.hxx"

#include <algorithm>

namespace mcode::model {

	auto role_from_string( const std::string_view text ) -> std::optional< role > {
		if ( text == "system" ) {
			return role::system;
		}

		if ( text == "user" ) {
			return role::user;
		}

		if ( text == "assistant" ) {
			return role::assistant;
		}

		if ( text == "tool" ) {
			return role::tool;
		}

		return std::nullopt;
	}

	auto message::text( ) const -> std::string {
		auto out = std::string{ };

		for ( const auto& piece : blocks ) {
			if ( piece.kind == block_kind::text || piece.kind == block_kind::thinking ) {
				out += piece.text;
			}
		}

		return out;
	}

	auto usage::add( const chat_event& event ) -> void {
		// Providers report usage two ways: as deltas per event, or once at the
		// end with cumulative totals. Treating the values as cumulative is the
		// safe reading -- a delta-accumulating provider that also emits a final
		// total would otherwise double-count.
		input = std::max( input, event.input_tokens );
		output = std::max( output, event.output_tokens );
		cached_read = std::max( cached_read, event.cached_read_tokens );
		cache_write = std::max( cache_write, event.cache_write_tokens );
		reasoning = std::max( reasoning, event.reasoning_tokens );
	}

	auto usage::total_tokens( ) const noexcept -> std::int64_t {
		// Cached reads are a subset of input at every provider that reports them,
		// so they are not added again.
		return input + output;
	}

	auto compute_cost( const capabilities& caps, const usage& counts ) -> double {
		constexpr auto PER_MILLION = 1'000'000.0;

		const auto billed_input = std::max< std::int64_t >( 0, counts.input - counts.cached_read );

		return ( static_cast< double >( billed_input ) * caps.price_input +
			static_cast< double >( counts.cached_read ) * caps.price_cached_read +
			static_cast< double >( counts.cache_write ) * caps.price_cache_write +
			static_cast< double >( counts.output ) * caps.price_output ) / PER_MILLION;
	}

}
