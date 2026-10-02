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
					// Tombstoned now, compacted when dispatch finishes: the slot must stay valid.
					entry_value.alive = false;
					entry_value.function = nullptr;
					entry_value.veto_function = nullptr;
				} else {
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

		// Snapshot the size: a mid-dispatch subscribe can reallocate, and is not called here.
		const auto count = list.size( );

		for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
			auto& entry_value = list[ index ];

			if ( !entry_value.alive ) {
				continue;
			}

			// A throwing handler is caught and counted, never auto-removed.
			try {
				if ( entry_value.veto_function ) {
					if ( auto decision = entry_value.veto_function( value ) ) {
						// First veto wins: later handlers are skipped for this dispatch.
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
		// Flat, never recursive, so the stack depth stays 1; the cap bounds a self-feeding handler.
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

			// A veto only answers the publish still unwinding, so a queued event's veto is dropped.
			dispatch_one( next );
		}
	}

	auto bus::publish( event value ) -> std::optional< veto > {
		if ( value.sequence == 0 && value.timestamp_ms == 0 ) {
			value.timestamp_ms = support::epoch_milliseconds( );
		}

		// Re-entrant publish queues instead of recursing, keeping the stack flat.
		if ( dispatching_ ) {
			pending_.push_back( std::move( value ) );

			return std::nullopt;
		}

		dispatching_ = true;

		const auto vetoed = dispatch_one( value );

		drain_pending( );

		dispatching_ = false;

		for ( auto& list : subscribers_ ) {
			list.erase( std::remove_if( list.begin( ), list.end( ), []( const entry& item ) {
				return !item.alive;
			} ), list.end( ) );
		}

		return vetoed;
	}

}
