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
		// usage arrives as per-event deltas or as one cumulative total; the max reads both right.
		input = std::max( input, event.input_tokens );
		output = std::max( output, event.output_tokens );
		cached_read = std::max( cached_read, event.cached_read_tokens );
		cache_write = std::max( cache_write, event.cache_write_tokens );
		reasoning = std::max( reasoning, event.reasoning_tokens );
	}

	auto total_tokens( const capabilities& caps, const usage& counts ) noexcept -> std::int64_t {
		// a provider reporting input without cache reads or writes left them out of the total.
		const auto extra_input = caps.cached_read_in_input
			? std::int64_t{ 0 }
			: counts.cached_read + counts.cache_write;

		return counts.input + extra_input + counts.output;
	}

	auto compute_cost( const capabilities& model_capabilities, const usage& counts ) -> double {
		constexpr auto PER_MILLION = 1'000'000.0;

		// subtracting cache reads from input a provider already excluded bills input at zero.
		const auto billed_input = model_capabilities.cached_read_in_input
			? std::max< std::int64_t >( 0, counts.input - counts.cached_read )
			: counts.input;

		return ( static_cast< double >( billed_input ) * model_capabilities.price_input +
			static_cast< double >( counts.cached_read ) * model_capabilities.price_cached_read +
			static_cast< double >( counts.cache_write ) * model_capabilities.price_cache_write +
			static_cast< double >( counts.output ) * model_capabilities.price_output ) / PER_MILLION;
	}

}
