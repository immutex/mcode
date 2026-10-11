#include "mcode/agent/loop.hxx"

#include "mcode/agent/context_controller.hxx"
#include "mcode/agent/loop_internal.hxx"
#include "mcode/agent/message_json.hxx"

#include <algorithm>
#include <map>
#include <utility>

#include "mcode/model/render.hxx"
#include "mcode/support/json.hxx"

namespace mcode {

	auto thrash_detector::record( const std::string_view tool_name, const std::string_view args_json )
		-> std::size_t {
		auto hashed = loop_internal::thrash_hash( tool_name, args_json );

		if ( !hashed ) {
			return 0;
		}

		auto repeats = std::size_t{ 1 };

		for ( auto index = window_.rbegin( ); index != window_.rend( ); ++index ) {
			if ( *index != *hashed ) {
				break;
			}

			++repeats;
		}

		window_.push_back( *hashed );

		if ( window_.size( ) > THRASH_WINDOW ) {
			window_.erase( window_.begin( ) );
		}

		current_repeats_ = repeats;

		return repeats;
	}

	auto assemble_request( const tool_registry& registry, const assemble_request_options& options )
		-> assembled_request {
		auto out = assembled_request{ };
		out.request.model = std::string{ options.model_name };
		out.request.cache.mode = options.mode;
		out.request.strict_tools = options.strict_tools;
		out.request.response_format_with_tools = options.response_format_with_tools;

		auto tools = registry.all( );
		std::sort( tools.begin( ), tools.end( ),
			[]( const tool_def* one, const tool_def* other ) {
				return one->name < other->name;
			} );

		for ( const auto* definition : tools ) {
			auto spec = model::tool_spec{ };
			spec.name = definition->name;
			spec.description = definition->description;
			spec.schema_json = definition->schema_json;

			out.request.tools.push_back( std::move( spec ) );
		}

		auto system = model::message{ };
		system.speaker = model::role::system;

		auto system_block = model::block{ };
		system_block.kind = model::block_kind::text;
		system_block.text = std::string{ options.system_prompt };
		system.blocks.push_back( std::move( system_block ) );

		out.request.messages.push_back( std::move( system ) );

		// Ahead of the history, so a compaction that drops turns cannot drop the goal.
		if ( !options.pinned_state.empty( ) ) {
			auto pinned = model::message{ };
			pinned.speaker = model::role::system;

			auto pinned_block = model::block{ };
			pinned_block.kind = model::block_kind::text;
			pinned_block.text = std::string{ options.pinned_state };
			pinned.blocks.push_back( std::move( pinned_block ) );

			out.request.messages.push_back( std::move( pinned ) );
		}

		for ( const auto& value : options.history ) {
			out.request.messages.push_back( value );
		}

		if ( options.near_budget ) {
			auto note = model::message{ };
			note.speaker = model::role::user;

			auto note_block = model::block{ };
			note_block.kind = model::block_kind::text;
			note_block.text = std::string{ loop_internal::NEAR_BUDGET_NOTE };
			note.blocks.push_back( std::move( note_block ) );

			out.request.messages.push_back( std::move( note ) );
		}

		// one breakpoint on the stable prefix (tools, then system) caches everything before it.
		if ( out.request.cache.mode == model::cache_mode::explicit_markers ) {
			out.request.cache.breakpoints = model::cache_breakpoints( out.request, options.fields );
		}

		return out;
	}

	auto agent_loop::tier_for( const model_tier tier ) const -> const tier_model& {
		switch ( tier ) {
			case model_tier::plan:
				return plan_tier_.configured ? plan_tier_ : default_tier_;

			case model_tier::side:
				return side_tier_.configured ? side_tier_ : default_tier_;

			case model_tier::act:
				break;
		}

		return act_tier_.configured ? act_tier_ : default_tier_;
	}

	auto agent_loop::working_model( ) const noexcept -> const model::capabilities& {
		return tier_for( model_tier::act ).caps;
	}

	// The side tier exists for exactly this: a mechanical call whose output feeds the
	// next turn, where a workhorse model is wasted and a failure costs nothing.
	auto agent_loop::summarize_dropped( const std::vector< model::message >& dropped )
		-> std::string {
		if ( dropped.empty( ) ) {
			return { };
		}

		const auto& routed = tier_for( model_tier::side );

		auto transcript = std::string{ };

		for ( const auto& value : dropped ) {
			transcript += agent::message_to_json( value );
			transcript += '\n';
		}

		auto request = model::chat_request{ };
		request.model = routed.name;

		auto system = model::message{ };
		system.speaker = model::role::system;

		auto system_block = model::block{ };
		system_block.kind = model::block_kind::text;
		system_block.text = std::string{ loop_internal::COMPACTION_SUMMARY_PROMPT };
		system.blocks.push_back( std::move( system_block ) );

		request.messages.push_back( std::move( system ) );

		auto user = model::message{ };
		user.speaker = model::role::user;

		auto user_block = model::block{ };
		user_block.kind = model::block_kind::text;
		user_block.text = std::move( transcript );
		user.blocks.push_back( std::move( user_block ) );

		request.messages.push_back( std::move( user ) );

		// No tools: this call summarizes, and a tool call here would have nowhere to
		// be dispatched.
		request.reasoning_effort = model::effort::low;

		auto stream_request = model::stream_request{ };
		stream_request.request = std::move( request );
		stream_request.provider = provider_;
		stream_request.api_key = api_key_;
		stream_request.caps = routed.caps;

		auto summary = std::string{ };
		auto folded = model::usage{ };

		const auto streamed = client_->stream( stream_request,
			[ & ]( const model::chat_event& event ) {
				if ( event.type == model::chat_event::kind::text_delta ) {
					summary += event.text;
				} else if ( event.type == model::chat_event::kind::usage ) {
					folded.add( event );
				}
			} );

		if ( !streamed ) {
			return { };
		}

		// The summary is a real spend: the run pays for it like any other call.
		run_usage_ += folded;
		budget_.charge( static_cast< std::uint64_t >( total_tokens( routed.caps, folded ) ),
			compute_cost( routed.caps, folded ) );

		return summary;
	}

	auto agent_loop::refresh_pinned_state( ) -> void {
		pinned_.task = task_;
		pinned_.plan = plan_;
		pinned_.modified_files = modified_files_;

		pinned_.open_errors.clear( );

		if ( !last_failure_.empty( ) ) {
			pinned_.open_errors.push_back( last_failure_ );
		}

		last_recitation_step_ = budget_.steps_used.load( );
	}

	// Rung 1 runs first: clearing costs no model call and carries most of the saving.
	auto agent_loop::maybe_compact( ) -> status {
		const auto plan = plan_context( history_, occupancy_, COMPACTION_KEEP_LAST_TOKENS );

		if ( plan.action == context_action::clear ) {
			const auto reclaimed = apply_clear( history_, plan.keep_last_results );

			if ( reclaimed > 0 ) {
				++occupancy_.epoch;

				auto payload = std::string{ "{\"reason\":\"tool-result-clear\",\"reclaimed_tokens\":" };
				payload += std::to_string( reclaimed );
				payload += ",\"kept_results\":";
				payload += std::to_string( plan.keep_last_results );
				payload += ",\"epoch\":";
				payload += std::to_string( occupancy_.epoch );
				payload += "}";

				log_->append( "context.compaction", payload );
				publish( events::kind::compaction, std::move( payload ) );

				// Summarizing now would pay a model call for nothing.
				if ( occupancy_.fill_fraction( ) < COMPACTION_TRIGGER_FRACTION ) {
					return status{ };
				}
			}
		}

		// `newest` below would underflow on an empty history.
		if ( history_.size( ) <= COMPACTION_KEEP_FIRST_EVENTS ) {
			return status{ };
		}

		const auto window_tokens = working_model( ).effective_context_window( );
		const auto reserved = static_cast< double >( RESERVED_OUTPUT_TOKENS );

		// Clamped: a declared window at or below the output reserve would never trigger.
		const auto usable = std::max< double >( static_cast< double >( window_tokens ) - reserved,
			static_cast< double >( window_tokens ) * MIN_USABLE_WINDOW_FRACTION );
		const auto window = usable * ( 1.0 - SAFETY_MARGIN_FRACTION );

		if ( window <= 0.0 ) {
			return status{ };
		}

		const auto fill = static_cast< double >( loop_internal::history_tokens( history_ ) );

		if ( fill < window * COMPACTION_TRIGGER_FRACTION ) {
			return status{ };
		}

		const auto ends_on_a_call = []( const model::message& value ) {
			for ( const auto& block : value.blocks ) {
				if ( block.kind == model::block_kind::tool_call ) {
					return true;
				}
			}

			return false;
		};

		const auto newest = history_.size( ) - 1;

		// the newest message is the turn's own answer, so it survives even when it alone is huge.
		auto tail_start = newest;
		auto tail_messages = std::size_t{ 1 };
		auto kept_tokens = loop_internal::message_tokens( history_[ newest ] );

		while ( tail_start > 0 && tail_messages < COMPACTION_KEEP_LAST_TURNS ) {
			const auto cost = loop_internal::message_tokens( history_[ tail_start - 1 ] );

			if ( kept_tokens + cost > COMPACTION_KEEP_LAST_TOKENS ) {
				break;
			}

			kept_tokens += cost;
			--tail_start;
			++tail_messages;
		}

		// a tool result cannot survive its assistant call, so the tail walks back over the pair.
		while ( tail_start > 0 && history_[ tail_start ].speaker == model::role::tool ) {
			--tail_start;
		}

		auto pinned = std::min( COMPACTION_KEEP_FIRST_EVENTS, history_.size( ) );

		if ( pinned > tail_start ) {
			pinned = tail_start;
		}

		// neither side of the cut may hold half a tool pair: walk the prefix back to a seam.
		while ( pinned > 0 && pinned < tail_start
			&& ( history_[ pinned ].speaker == model::role::tool
				|| ends_on_a_call( history_[ pinned - 1 ] ) ) ) {
			--pinned;
		}

		// Prefix and tail can meet, which would report a drop that did not occur.
		// Decided before the summary call, so a no-op compaction costs no model call.
		if ( tail_start <= pinned ) {
			return status{ };
		}

		auto dropped_messages = std::vector< model::message >{ };

		for ( auto index = pinned; index < tail_start; ++index ) {
			dropped_messages.push_back( history_[ index ] );
		}

		// The middle is compressed rather than silently dropped: what is lost is the
		// narration, not the paths and the errors. A failed call falls back to the
		// drop, which is lossy but never loses the goal — that lives in pinned state.
		auto summary = summarize_dropped( dropped_messages );

		auto kept = std::vector< model::message >{ };

		for ( auto index = std::size_t{ 0 }; index < pinned; ++index ) {
			kept.push_back( history_[ index ] );
		}

		if ( !summary.empty( ) ) {
			auto note = model::message{ };
			note.speaker = model::role::system;

			auto note_block = model::block{ };
			note_block.kind = model::block_kind::text;
			note_block.text = "Earlier in this session, compressed:\n" + summary;
			note.blocks.push_back( std::move( note_block ) );

			kept.push_back( std::move( note ) );
		}

		for ( auto index = tail_start; index < history_.size( ); ++index ) {
			kept.push_back( history_[ index ] );
		}

		const auto dropped = dropped_messages.size( );

		history_ = std::move( kept );

		auto payload = std::string{ "{\"reason\":\"80pct\",\"kept_tokens\":" };
		payload += std::to_string( kept_tokens );
		payload += ",\"dropped_messages\":";
		payload += std::to_string( dropped );
		payload += ",\"summarized\":";
		payload += summary.empty( ) ? "false" : "true";
		payload += "}";

		log_->append( "context.compaction", payload );
		publish( events::kind::compaction, std::move( payload ) );

		return status{ };
	}

	auto build_system_prompt( const tool_registry& registry,
		const std::string_view instruction_chain,
		const std::string_view skill_index,
		const std::string_view notes_pointer ) -> std::string {
		auto has_tool = [&]( const std::string_view name ) {
			return registry.find( name ) != nullptr;
		};

		auto out = std::string{ };

		out += "# Identity\n";
		out += "You are mcode, a coding agent working in a user's repository.\n\n";

		out += "# Persistence\n";
		out += "Keep going until the request is resolved. Only stop when the task is done";
		out += " or a blocker needs the user.\n\n";

		out += "# Scope discipline\n";
		out += "Deliver the full requested scope. Do not quietly narrow or widen it.";
		out += " Make routine judgment calls; ask only when a wrong assumption is unsafe";
		out += " or makes the work useless.\n\n";

		out += "# Tool usage\n";
		out += "Prefer the dedicated tool over a shell command. Batch independent reads";
		out += " in one turn; never parallel-edit the same file.\n";

		if ( has_tool( "read" ) && has_tool( "edit" ) ) {
			out += "Read a file before editing it.\n";
		}

		out += "\n# Coding conventions\n";
		out += "Mimic the existing style of the files you touch. Default to no comments;";
		out += " where one is justified, keep it to one line. Do not add speculative";
		out += " abstractions, impossible-scenario handling, or compatibility shims.\n\n";

		out += "# Verification\n";
		out += "Run the change's own test or command before claiming success. State";
		out += " failures with their output; never hedge on verified work.\n\n";

		out += "# Safety\n";
		out += "Never revert changes you did not make. Stop and ask when the repository";
		out += " changes unexpectedly. Do not force-push or edit git config.\n\n";

		out += "# Output format\n";
		out += "Keep chat terse; keep code at full verbosity. Cite changes as";
		out += " path:line.\n";

		if ( !instruction_chain.empty( ) ) {
			out += "\n# Project instructions\n";
			out += "The instructions below come from the repository's AGENTS.md";
			out += " chain, broadest first. When instructions contradict, the";
			out += " CLOSEST one to your current work wins; later text overrides";
			out += " earlier text.\n\n";
			out += instruction_chain;

			if ( !instruction_chain.ends_with( '\n' ) ) {
				out += '\n';
			}
		}

		if ( !skill_index.empty( ) && has_tool( "skill_read" ) ) {
			out += "\n# Skills\n";
			out += "Procedures available as skills. When one matches the task,";
			out += " read its full instructions with the skill_read tool before";
			out += " acting on it.\n\n";
			out += skill_index;

			if ( !skill_index.ends_with( '\n' ) ) {
				out += '\n';
			}
		}

		// A pointer, not content: an always-injected ledger block is the design that
		// measured worst for poisoning.
		if ( !notes_pointer.empty( ) && has_tool( "memory_search" ) ) {
			out += "\n# Notes\n";
			out += notes_pointer;

			if ( !notes_pointer.ends_with( '\n' ) ) {
				out += '\n';
			}
		}

		return out;
	}


} // namespace mcode
