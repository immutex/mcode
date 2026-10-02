#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	// stateful: a provider splits a call's arguments across arbitrary events, keyed by index.
	class delta_applier {
	public:
		explicit delta_applier( const provider_descriptor& descriptor );
		explicit delta_applier( provider_descriptor&& descriptor );

		using escape_hatch = std::function< result< std::vector< chat_event > >(
			std::string_view event_name, std::string_view data ) >;

		auto set_escape_hatch( escape_hatch callback ) -> void;

		// a malformed payload is an error, not a silent skip.
		[[nodiscard]] auto feed( std::string_view event_name, std::string_view data )
			-> result< std::vector< chat_event > >;

		[[nodiscard]] auto finish( ) -> std::vector< chat_event >;

		[[nodiscard]] auto saw_terminal_event( ) const noexcept -> bool { return terminal_seen_; }
		[[nodiscard]] auto accumulated_usage( ) const noexcept -> const usage& { return usage_; }

	private:
		// bounds the wire-supplied index, which is otherwise an amplification primitive.
		inline static constexpr std::int64_t MAX_PARALLEL_CALLS = 256;

		struct pending_call {
			int index = 0;
			std::string id;
			std::string name;
			std::string args_fragments;
		};

		auto pending_for( const int index ) -> pending_call&;

		provider_descriptor descriptor_;
		escape_hatch escape_;
		std::vector< pending_call > pending_;
		usage usage_;
		std::string stop_reason_;
		bool terminal_seen_ = false;
	};

}
