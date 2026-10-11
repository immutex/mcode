#include "mcode/agent/context_controller.hxx"

#include <algorithm>

#include "mcode/agent/loop_internal.hxx"
#include "mcode/model/types.hxx"

namespace mcode {

	auto context_occupancy::fill_fraction( ) const noexcept -> double {
		if ( usable_window <= 0 ) {
			return 0.0;
		}

		return static_cast< double >( prompt_tokens ) / static_cast< double >( usable_window );
	}

	auto context_occupancy::cache_hit_ratio( ) const noexcept -> double {
		const auto denom = prompt_tokens;

		if ( denom <= 0 ) {
			return 0.0;
		}

		return static_cast< double >( cached_read ) / static_cast< double >( denom );
	}

	auto plan_context( const std::vector< model::message >& history,
		const context_occupancy& occupancy, const std::int64_t keep_last_tokens ) -> context_plan {
		auto plan = context_plan{ };

		if ( occupancy.usable_window <= 0 || history.empty( ) ) {
			return plan;
		}

		if ( occupancy.fill_fraction( ) < TOOL_CLEAR_TRIGGER_FRACTION ) {
			return plan;
		}

		// Only results are candidates: the call record is what lets the model re-issue
		// the read, so dropping it would remove the path back to the payload.
		auto kept_tokens = std::int64_t{ 0 };
		auto keep_results = std::size_t{ 0 };
		auto index = history.size( );

		while ( index > 0 ) {
			--index;

			kept_tokens += loop_internal::message_tokens( history[ index ] );

			for ( const auto& block : history[ index ].blocks ) {
				if ( block.kind == model::block_kind::tool_result ) {
					++keep_results;
				}
			}

			if ( kept_tokens >= keep_last_tokens ) {
				break;
			}
		}

		auto reclaimable = std::int64_t{ 0 };

		for ( auto before = std::size_t{ 0 }; before < index; ++before ) {
			for ( const auto& block : history[ before ].blocks ) {
				if ( block.kind == model::block_kind::tool_result ) {
					reclaimable += loop_internal::token_estimate( block.result_json );
				}
			}
		}

		const auto floor = static_cast< std::int64_t >(
			static_cast< double >( occupancy.usable_window ) * CLEAR_AT_LEAST_FRACTION );

		if ( reclaimable < floor ) {
			return plan;
		}

		plan.action = context_action::clear;
		plan.keep_last_results = keep_results;

		return plan;
	}

	auto apply_clear( std::vector< model::message >& history, const std::size_t keep ) -> std::int64_t {
		auto reclaimable = std::vector< std::pair< model::block*, std::int64_t > >{ };

		for ( auto& message : history ) {
			for ( auto& block : message.blocks ) {
				if ( block.kind == model::block_kind::tool_result && !block.result_json.empty( ) ) {
					reclaimable.emplace_back( &block,
						loop_internal::token_estimate( block.result_json ) );
				}
			}
		}

		if ( reclaimable.size( ) <= keep ) {
			return 0;
		}

		auto reclaimed = std::int64_t{ 0 };

		// Recent output is what the current step is reasoning about.
		for ( auto index = std::size_t{ 0 }; index < reclaimable.size( ) - keep; ++index ) {
			auto* block = reclaimable[ index ].first;

			auto stub = std::string{ CLEARED_RESULT_PREFIX };
			stub += std::to_string( reclaimable[ index ].second );
			stub += " tokens of tool output; re-run the call to see it again]";

			block->result_json = std::move( stub );
			reclaimed += reclaimable[ index ].second;
		}

		return reclaimed;
	}

	auto render_pinned_state( const pinned_state& state ) -> std::string {
		if ( state.task.empty( ) && state.modified_files.empty( ) && state.plan.empty( )
			&& state.open_errors.empty( ) ) {
			return { };
		}

		auto out = std::string{ };

		if ( !state.task.empty( ) ) {
			out += "Task: ";
			out += state.task;
			out += '\n';
		}

		if ( !state.plan.empty( ) ) {
			out += "Plan: ";
			out += state.plan;
			out += '\n';
		}

		if ( !state.modified_files.empty( ) ) {
			out += "Modified: ";

			for ( const auto& path : state.modified_files ) {
				out += path;
				out += ' ';
			}

			out += '\n';
		}

		if ( !state.open_errors.empty( ) ) {
			out += "Unresolved: ";

			for ( const auto& failure : state.open_errors ) {
				out += failure;
				out += "; ";
			}

			out += '\n';
		}

		return out;
	}

}
