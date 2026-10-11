#include "mcode/agent/loop.hxx"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/agent/message_json.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/support/json.hxx"

namespace mcode {

	auto agent_loop::register_handler( const std::string name, tool_handler handler ) -> void {
		handlers_.insert_or_assign( std::move( name ), std::move( handler ) );
	}

	auto agent_loop::run_end_payload( const std::string_view reason ) const -> std::string {
		auto summary = std::string{ "{\"goal\":\"" };
		json::append_escaped( summary, user_task_ );
		summary += "\",\"actions\":";
		summary += std::to_string( budget_.steps_used.load( ) );
		summary += ",\"last_failure\":\"";
		json::append_escaped( summary, last_failure_ );
		summary += "\",\"remaining_steps\":";
		summary += std::to_string( budget_.max_steps > budget_.steps_used.load( )
				? budget_.max_steps - budget_.steps_used.load( )
				: 0 );

		// No leading quote: the previous field is a number, so a `"` here would close
		// a string that was never open.
		summary += ",\"remaining_usd\":";
		summary += std::to_string( budget_.max_usd > budget_.usd_used.load( )
				? budget_.max_usd - budget_.usd_used.load( )
				: 0.0 );
		summary += ",\"input_tokens\":";
		summary += std::to_string( run_usage_.input );
		summary += ",\"output_tokens\":";
		summary += std::to_string( run_usage_.output );
		summary += ",\"cached_read_tokens\":";
		summary += std::to_string( run_usage_.cached_read );
		summary += ",\"cache_write_tokens\":";
		summary += std::to_string( run_usage_.cache_write );
		summary += ",\"reasoning_tokens\":";
		summary += std::to_string( run_usage_.reasoning );
		summary += ",\"state\":\"";
		summary += to_string( state_ );
		summary += "\",\"verification\":\"";
		summary += to_string( verdict_ );

		if ( !verification_detail_.empty( ) ) {
			summary += "\",\"verification_detail\":\"";
			json::append_escaped( summary, verification_detail_ );
		}

		summary += "\",\"reason\":\"";
		json::append_escaped( summary, reason );

		summary += "\"}";

		return summary;
	}

	auto agent_loop::finish_run( const loop_state terminal, const std::string_view reason )
		-> void {
		state_ = terminal;
		end_reason_ = std::string{ reason };

		auto summary = run_end_payload( reason );

		log_->append( "run.end", summary );

		// The bus has a kind for a failed run that nothing else publishes.
		if ( terminal == loop_state::failed ) {
			publish( events::kind::error, std::move( summary ) );
		}
	}

	auto agent_loop::seed_history( std::vector< model::message > restored ) -> void {
		history_ = std::move( restored );
	}

	auto agent_loop::reset_session( std::vector< model::message > restored ) -> void {
		history_ = std::move( restored );

		budget_.steps_used.store( 0 );
		budget_.tokens_used.store( 0 );
		budget_.usd_used.store( 0.0 );

		mint_run_id( );

		state_ = loop_state::idle;
	}

	auto agent_loop::approval_mode( ) const noexcept -> std::string_view {
		return permissions_ != nullptr ? permissions_->approval_mode( ) : std::string_view{ };
	}

}
