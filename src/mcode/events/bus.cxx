#include "mcode/events/bus.hxx"

#include "mcode/support/time.hxx"

namespace mcode::events {

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

		// Indexed, against a snapshot of the size. A handler may SUBSCRIBE during
		// dispatch -- a hook that installs another hook is ordinary -- and that
		// push_back can reallocate, which would invalidate a range-for's iterators.
		// The snapshot also means an entry added mid-dispatch is not called for the
		// event that caused its own registration.
		const auto count = list.size( );

		for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
			auto& entry_value = list[ index ];

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
		// to the same deque and handled in the same loop, so the stack depth stays 1
		// no matter how many events a handler emits.
		//
		// Bounded, because "flat" is not the same as "terminating". A handler that
		// publishes an event of the kind it subscribes to appends work every
		// iteration, and the deque would grow until memory ran out. The cap turns
		// that into a counted drop.
		auto drained = std::size_t{ 0 };

		while ( !pending_.empty( ) ) {
			if ( drained >= MAX_EVENTS_PER_PUBLISH ) {
				++overflow_drops_;
				pending_.clear( );

				break;
			}

			auto next = std::move( pending_.front( ) );
			pending_.pop_front( );
			++drained;

			// The veto is dropped, and that is correct: a veto answers the publish
			// that is currently unwinding. A handler that queues an event during
			// drain has already returned, so there is no call left for it to refuse.
			dispatch_one( next );
		}
	}

	auto bus::publish( event value ) -> std::optional< veto > {
		if ( value.sequence == 0 && value.timestamp_ms == 0 ) {
			value.timestamp_ms = support::epoch_milliseconds( );
		}

		// Re-entrant publish: queue it. The outer drain will handle it, which keeps
		// the stack flat and makes infinite recursion impossible by construction
		// rather than by convention.
		if ( dispatching_ ) {
			pending_.push_back( std::move( value ) );

			return std::nullopt;
		}

		dispatching_ = true;

		const auto vetoed = dispatch_one( value );

		drain_pending( );

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
