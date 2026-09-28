#include "mcode/events/bus.hxx"

#include <chrono>

namespace mcode::events {

	namespace {

		// Neovim's E218 number. A hook chain deeper than this is a bug, and a cap
		// makes the failure loud rather than a stack overflow.
		constexpr auto MAX_DISPATCH_DEPTH = std::size_t{ 10 };

		auto now_ms( ) -> std::int64_t {
			return std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::system_clock::now( ).time_since_epoch( ) ).count( );
		}

	}

	auto bus::subscribe( const kind type, handler function ) -> subscription_id {
		auto entry_value = entry{ };
		entry_value.id = next_id_++;
		entry_value.function = std::move( function );

		auto& list = subscribers_[ static_cast< std::size_t >( type ) ];
		list.push_back( std::move( entry_value ) );

		return list.back( ).id;
	}

	auto bus::subscribe_veto( const kind type, veto_handler function ) -> subscription_id {
		auto entry_value = entry{ };
		entry_value.id = next_id_++;
		entry_value.veto_function = std::move( function );

		auto& list = subscribers_[ static_cast< std::size_t >( type ) ];
		list.push_back( std::move( entry_value ) );

		return list.back( ).id;
	}

	auto bus::unsubscribe( const subscription_id id ) -> void {
		for ( auto& list : subscribers_ ) {
			for ( auto& entry_value : list ) {
				if ( entry_value.id != id ) {
					continue;
				}

				if ( dispatching_ ) {
					// Mid-dispatch removal must not invalidate iteration, so the slot
					// is tombstoned now and compacted when dispatch finishes.
					entry_value.alive = false;
					entry_value.function = nullptr;
					entry_value.veto_function = nullptr;
				} else {
					// O(1) swap-remove.
					entry_value = std::move( list.back( ) );
					list.pop_back( );
				}

				return;
			}
		}
	}

	auto bus::subscriber_count( const kind type ) const noexcept -> std::size_t {
		const auto& list = subscribers_[ static_cast< std::size_t >( type ) ];

		return static_cast< std::size_t >(
			std::count_if( list.begin( ), list.end( ), []( const entry& item ) {
				return item.alive;
			} ) );
	}

	auto bus::dispatch_one( const event& value ) -> std::optional< veto > {
		auto& list = subscribers_[ static_cast< std::size_t >( value.type ) ];

		for ( auto& entry_value : list ) {
			if ( !entry_value.alive ) {
				continue;
			}

			// Dispatch is noexcept at the bus boundary. A throwing handler is caught
			// and counted rather than allowed to abort dispatch, and it is NOT
			// auto-removed -- removal would make a transient bug permanent.
			try {
				if ( entry_value.veto_function ) {
					if ( auto decision = entry_value.veto_function( value ) ) {
						// First veto wins and short-circuits: later handlers for this
						// event are skipped for this dispatch.
						return decision;
					}

					continue;
				}

				if ( entry_value.function ) {
					entry_value.function( value );
				}
			} catch ( ... ) {
				++handler_failures_;
			}
		}

		return std::nullopt;
	}

	auto bus::drain_pending( ) -> void {
		// Flat, never recursive: everything published during this drain is appended
		// to the same deque and handled in the same loop, so nesting depth stays 1
		// no matter how many events a handler emits.
		while ( !pending_.empty( ) ) {
			auto next = std::move( pending_.front( ) );
			pending_.pop_front( );

			// A nested publish sees depth_ > 0 and re-queues, so this loop is the
			// only place dispatch_one is called at depth 1.
			const auto vetoed = dispatch_one( next );
			(void)vetoed;
		}
	}

	auto bus::publish( event value ) -> std::optional< veto > {
		if ( value.sequence == 0 && value.timestamp_ms == 0 ) {
			value.timestamp_ms = now_ms( );
		}

		// Re-entrant publish: queue it. The outer drain will handle it, which keeps
		// the stack flat and makes infinite recursion impossible by construction
		// rather than by convention.
		if ( dispatching_ ) {
			if ( depth_ >= MAX_DISPATCH_DEPTH ) {
				++handler_failures_;

				return std::nullopt;
			}

			pending_.push_back( std::move( value ) );

			return std::nullopt;
		}

		dispatching_ = true;
		depth_ = 1;

		if ( depth_ > max_depth_ ) {
			max_depth_ = depth_;
		}

		const auto vetoed = dispatch_one( value );

		drain_pending( );

		depth_ = 0;
		dispatching_ = false;

		// Compact every list that gained a tombstone during this dispatch.
		for ( auto& list : subscribers_ ) {
			list.erase( std::remove_if( list.begin( ), list.end( ), []( const entry& item ) {
				return !item.alive;
			} ), list.end( ) );
		}

		return vetoed;
	}

}
