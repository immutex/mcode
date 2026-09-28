#include "mcode/agent/loop.hxx"

#include <algorithm>
#include <exception>
#include <fstream>
#include <map>

#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"

#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace mcode {

	namespace {

		inline constexpr std::string_view NEAR_BUDGET_NOTE =
			"budget nearly exhausted - wrap up or report blockers";

		auto token_estimate( const std::string_view text ) noexcept -> std::int64_t {
			return static_cast< std::int64_t >( text.size( ) / CHARS_PER_TOKEN_ESTIMATE );
		}

		auto message_tokens( const model::message& value ) -> std::int64_t {
			auto total = std::int64_t{ 0 };

			for ( const auto& block : value.blocks ) {
				total += token_estimate( block.text );
				total += token_estimate( block.args_json );
				total += token_estimate( block.result_json );
			}

			return total;
		}

		auto history_tokens( const std::vector< model::message >& history ) -> std::int64_t {
			auto total = std::int64_t{ 0 };

			for ( const auto& value : history ) {
				total += message_tokens( value );
			}

			return total;
		}

		auto thrash_hash( const std::string_view tool_name, const std::string_view args_json )
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

			auto result_block( const tool_outcome& outcome ) -> model::block {
			auto block = model::block{ };
			block.kind = model::block_kind::tool_result;
			block.is_error = !outcome.ok;
			block.result_json = outcome.ok ? outcome.content : outcome.error_message;

			return block;
		}

			auto collect_calls( const std::vector< model::chat_event >& events )
			-> std::vector< tool_call > {
			auto calls = std::vector< tool_call >{ };
			auto args = std::map< int, std::string >{ };
			auto names = std::map< int, std::string >{ };

			for ( const auto& event : events ) {
				switch ( event.type ) {
					case model::chat_event::kind::tool_call_delta: {
						names[ event.index ] = event.tool_name;
						args[ event.index ] += event.args_fragment;

						break;
					}

					default: break;
				}
			}

			for ( const auto& [ index, name ] : names ) {
				calls.push_back( { name, args.contains( index ) ? args.at( index ) : "{}" } );
			}

			return calls;
		}

		} // namespace

	auto to_string( const loop_state value ) noexcept -> std::string_view {
		switch ( value ) {
			case loop_state::idle: return "idle";
			case loop_state::plan: return "plan";
			case loop_state::act: return "act";
			case loop_state::observe: return "observe";
			case loop_state::verify: return "verify";
			case loop_state::reflect: return "reflect";
			case loop_state::replan: return "replan";
			case loop_state::handoff: return "handoff";
			case loop_state::done: return "done";
			case loop_state::failed: return "failed";
		}

		return "idle";
	}

	auto thrash_detector::record( const std::string_view tool_name, const std::string_view args_json )
		-> std::size_t {
		auto hashed = thrash_hash( tool_name, args_json );

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

	auto assemble_request( const tool_registry& registry, const std::string_view system_prompt,
		const std::vector< model::message >& history, const std::string_view model_name,
		const model::capabilities&, const model::cache_mode mode, const bool near_budget,
		const std::string_view recitation ) -> assembled_request {
		auto out = assembled_request{ };
		out.request.model = std::string{ model_name };
		out.request.cache.mode = mode;

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
		system_block.text = std::string{ system_prompt };
		system.blocks.push_back( std::move( system_block ) );

		out.request.messages.push_back( std::move( system ) );

		for ( const auto& value : history ) {
			out.request.messages.push_back( value );
		}

		if ( !recitation.empty( ) ) {
			auto tail = model::message{ };
			tail.speaker = model::role::user;

			auto tail_block = model::block{ };
			tail_block.kind = model::block_kind::text;
			tail_block.text = std::string{ recitation };
			tail.blocks.push_back( std::move( tail_block ) );

			out.request.messages.push_back( std::move( tail ) );
		}

		if ( near_budget ) {
			auto note = model::message{ };
			note.speaker = model::role::user;

			auto note_block = model::block{ };
			note_block.kind = model::block_kind::text;
			note_block.text = std::string{ NEAR_BUDGET_NOTE };
			note.blocks.push_back( std::move( note_block ) );

			out.request.messages.push_back( std::move( note ) );
			out.near_budget_note = true;
		}

		out.prefix_bytes = out.request.messages.size( ) > 0 ? 1 : 0;

		return out;
	}

	auto build_system_prompt( const tool_registry& registry ) -> std::string {
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

		return out;
	}

	agent_loop::agent_loop( dependencies dependencies )
		: registry_( dependencies.registry ), client_( dependencies.client ),
		log_( dependencies.log ), bus_( dependencies.bus ), budget_( dependencies.budget ),
		model_name_( dependencies.model_name ), caps_( dependencies.caps ),
		provider_( dependencies.provider ), api_key_( dependencies.api_key ),
		workspace_root_( dependencies.workspace_root ),
		platform_name_( dependencies.platform_name ) {
		if ( registry_ == nullptr ) {
			registry_ = &owned_registry_;
		}

		if ( log_ == nullptr ) {
			log_ = &owned_log_;
		}

		if ( bus_ == nullptr ) {
			bus_ = &owned_bus_;
		}
	}

	auto agent_loop::register_handler( std::string name, tool_handler handler ) -> void {
		handlers_.insert_or_assign( std::move( name ), std::move( handler ) );
	}

	auto agent_loop::observe_result( const tool_call& call, const tool_outcome& outcome ) -> void {
		auto message = model::message{ };
		message.speaker = model::role::tool;
		message.blocks.push_back( result_block( outcome ) );

		history_.push_back( std::move( message ) );

		auto payload = std::string{ "{\"tool\":\"" };
		json::append_escaped( payload, call.name );
		payload += "\",\"ok\":";
		payload += outcome.ok ? "true" : "false";
		payload += "}";

		log_->append( "tool.result", std::move( payload ) );
	}

	auto agent_loop::request_and_fold( const model::effort effort ) -> result< bool > {
		const auto near_budget = budget_.nearly_exhausted( );
		const auto assembled = assemble_request( *registry_, build_system_prompt( *registry_ ),
			history_, model_name_, caps_, caps_.caching, near_budget, { } );

		auto stream_request = model::stream_request{ };
		stream_request.request = assembled.request;
		stream_request.request.reasoning_effort = effort;
		stream_request.provider = provider_;
		stream_request.api_key = api_key_;
		stream_request.caps = caps_;

		auto events = std::vector< model::chat_event >{ };
		auto text = std::string{ };

		const auto streamed = client_->stream( stream_request,
			[ & ]( const model::chat_event& event ) {
				events.push_back( event );

				if ( event.type == model::chat_event::kind::text_delta ) {
					text += event.text;
				}
			} );

		if ( !streamed ) {
			return std::unexpected( streamed.error( ) );
		}

		auto folded = model::usage{ };
		double cost = 0.0;

		for ( const auto& event : events ) {
			if ( event.type == model::chat_event::kind::usage ) {
				folded.add( event );
			}
		}

		cost = compute_cost( caps_, folded );
		budget_.charge( static_cast< std::uint64_t >( folded.total_tokens( ) ), cost );

		auto assistant = model::message{ };
		assistant.speaker = model::role::assistant;

		if ( !text.empty( ) ) {
			auto block = model::block{ };
			block.kind = model::block_kind::text;
			block.text = std::move( text );
			assistant.blocks.push_back( std::move( block ) );
		}

		pending_calls_ = collect_calls( events );

		const auto& calls = pending_calls_;

		for ( const auto& call : calls ) {
			auto block = model::block{ };
			block.kind = model::block_kind::tool_call;
			block.tool_name = call.name;
			block.args_json = call.args_json;
			assistant.blocks.push_back( std::move( block ) );
		}

		history_.push_back( std::move( assistant ) );

		return !calls.empty( );
	}

	auto agent_loop::dispatch_calls( const std::vector< tool_call >& calls ) -> bool {
		auto hard_error = false;

		for ( const auto& call : calls ) {
			thrash_.record( call.name, call.args_json );

			auto outcome = execute( call );
			observe_result( call, outcome );

			if ( !outcome.ok ) {
				hard_error = true;
				last_failure_ = outcome.error_message;
			}
		}

		return hard_error;
	}

	auto agent_loop::maybe_compact( ) -> status {
		const auto usable = static_cast< double >( caps_.context_window - RESERVED_OUTPUT_TOKENS );
		const auto window = usable * ( 1.0 - SAFETY_MARGIN_FRACTION );
		const auto fill = static_cast< double >( history_tokens( history_ ) );

		if ( caps_.context_window == 0 || fill < window * COMPACTION_TRIGGER_FRACTION ) {
			return status{ };
		}

		auto result = compaction_result{ };
		result.pinned_facts.push_back( user_task_ );

		auto keep_first = std::size_t{ 0 };
		auto kept_tokens = std::int64_t{ 0 };

		for ( auto index = history_.size( ); index > COMPACTION_KEEP_FIRST_EVENTS; --index ) {
			const auto cost = message_tokens( history_[ index - 1 ] );

			if ( kept_tokens + cost > COMPACTION_KEEP_LAST_TOKENS ||
				history_.size( ) - index >= COMPACTION_KEEP_LAST_TURNS ) {
				break;
			}

			kept_tokens += cost;
			keep_first = index;
		}

		for ( auto index = keep_first; index < history_.size( ); ++index ) {
			result.kept.push_back( history_[ index ] );
		}

		history_ = std::move( result.kept );

		auto payload = std::string{ "{\"reason\":\"80pct\",\"kept_tokens\":" };
		payload += std::to_string( kept_tokens );
		payload += "}";

		log_->append( "context.compaction", std::move( payload ) );

		return status{ };
	}

	auto agent_loop::finish_run( const loop_state terminal, const std::string_view reason )
		-> void {
		state_ = terminal;

		auto summary = std::string{ "{\"goal\":\"" };
		json::append_escaped( summary, user_task_ );
		summary += "\",\"actions\":";
		summary += std::to_string( budget_.steps_used );
		summary += ",\"last_failure\":\"";
		json::append_escaped( summary, last_failure_ );
		summary += "\",\"remaining_steps\":";
		summary += std::to_string( budget_.max_steps > budget_.steps_used
				? budget_.max_steps - budget_.steps_used
				: 0 );
		summary += ",\"remaining_usd\":";
		summary += std::to_string( budget_.max_usd > budget_.usd_used
				? budget_.max_usd - budget_.usd_used
				: 0.0 );
		summary += ",\"state\":\"";
		summary += to_string( terminal );
		summary += "\",\"reason\":\"";
		json::append_escaped( summary, reason );
		summary += "\"}";

		log_->append( "run.end", summary );
	}

	auto agent_loop::run( const std::string_view user_task ) -> result< turn_outcome > {
		user_task_ = std::string{ user_task };
		visited_.clear( );
		pending_calls_.clear( );
		thrash_ = thrash_detector{ };
		reflection_counts_.clear( );
		total_reflections_ = 0;
		last_failure_.clear( );
		replan_count_ = 0;
		last_failure_repeats_ = 0;

		auto task_message = model::message{ };
		task_message.speaker = model::role::user;

		auto task_block = model::block{ };
		task_block.kind = model::block_kind::text;
		task_block.text = user_task_;
		task_message.blocks.push_back( std::move( task_block ) );

		history_.push_back( std::move( task_message ) );

		return run_state_machine( );
	}

	auto agent_loop::run_state_machine( ) -> turn_outcome {
		auto outcome = turn_outcome{ };

		state_ = loop_state::plan;

		while ( true ) {
			visited_.push_back( state_ );

			switch ( state_ ) {
				case loop_state::plan: {
					if ( budget_.steps_used >= budget_.max_steps ) {
						state_ = loop_state::failed;

						break;
					}

					auto planned = request_and_fold( model::effort::high );

					if ( !planned ) {
						finish_run( loop_state::failed, planned.error( ).msg );
						outcome.final_state = state_;
						outcome.visited = visited_;
						outcome.summary_json = planned.error( ).msg;

						return outcome;
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::act: {
					if ( budget_.exhausted( ) ) {
						finish_run( loop_state::handoff, "budget exhausted" );
						state_ = loop_state::handoff;

						break;
					}

					auto acted = request_and_fold( model::effort::medium );

					if ( !acted ) {
						finish_run( loop_state::handoff, acted.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					if ( *acted ) {
						const auto pending = pending_calls_;
						pending_calls_.clear( );
						dispatch_calls( pending );

						state_ = loop_state::observe;

						break;
					}

					state_ = loop_state::verify;

					break;
				}

				case loop_state::observe: {
					if ( budget_.exhausted( ) ) {
						finish_run( loop_state::handoff, "budget exhausted" );
						state_ = loop_state::handoff;

						break;
					}

					const auto repeats = thrash_.repeat_count( );

					if ( repeats >= 2 * THRASH_REPEAT_LIMIT ) {
						++replan_count_;

						if ( replan_count_ >= 2 ) {
							finish_run( loop_state::handoff, "thrash beyond replan guard" );
							state_ = loop_state::handoff;

							break;
						}

						state_ = loop_state::replan;

						break;
					}

					if ( repeats >= THRASH_REPEAT_LIMIT ) {
						if ( total_reflections_ >= MAX_REFLECTIONS_PER_RUN ) {
							state_ = loop_state::replan;

							break;
						}

						state_ = loop_state::reflect;

						break;
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::verify: {
					state_ = loop_state::handoff;

					break;
				}

				case loop_state::reflect: {
					const auto failure_class = last_failure_.empty( )
						? std::string{ "thrash" }
						: last_failure_;

					auto& count = reflection_counts_[ failure_class ];

					if ( count >= MAX_REFLECTIONS_PER_FAILURE_CLASS ) {
						state_ = loop_state::replan;

						break;
					}

					++count;
					++total_reflections_;

					auto reflected = request_and_fold( model::effort::high );

					if ( !reflected ) {
						finish_run( loop_state::handoff, reflected.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					if ( *reflected ) {
						++last_failure_repeats_;

						if ( last_failure_repeats_ >= 2 ) {
							state_ = loop_state::replan;

							break;
						}
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::replan: {
					if ( replan_count_ >= 2 ) {
						finish_run( loop_state::handoff, "no new plan" );
						state_ = loop_state::handoff;

						break;
					}

					++replan_count_;

					auto replanned = request_and_fold( model::effort::high );

					if ( !replanned ) {
						finish_run( loop_state::handoff, replanned.error( ).msg );
						state_ = loop_state::handoff;

						break;
					}

					state_ = loop_state::act;

					break;
				}

				case loop_state::handoff:
				case loop_state::done: {
					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.model_calls = budget_.steps_used;

					return outcome;
				}

				case loop_state::failed: {
					finish_run( loop_state::failed, "budget exhausted during plan" );
					outcome.final_state = state_;
					outcome.visited = visited_;
					outcome.summary_json = "budget exhausted during plan";

					return outcome;
				}

				case loop_state::idle: {
					state_ = loop_state::plan;

					break;
				}
			}

			if ( auto compacted = maybe_compact( ); !compacted ) {
				finish_run( loop_state::handoff, compacted.error( ).msg );
				state_ = loop_state::handoff;
			}
		}
	}

	auto agent_loop::execute( const tool_call& call ) -> tool_outcome {
		const auto started = std::chrono::steady_clock::now( );

		auto outcome = tool_outcome{ };

		const auto finish = [&]( ) {
			outcome.elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ) - started );

			return outcome;
		};

		{
			auto payload = std::string{ "{\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"args\":";
			payload += call.args_json.empty( ) ? "{}" : call.args_json;
			payload += "}";

			log_->append( "tool.call", std::move( payload ) );
		}

		if ( budget_.exhausted( ) ) {
			outcome.ok = false;
			outcome.code = errc::budget_exhausted;
			outcome.error_message = "session budget exhausted";
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"budget_exhausted\"}" );

			return finish( );
		}

		const auto* definition = registry_->find( call.name );

		if ( definition == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "unknown tool: " + call.name;

			auto payload = std::string{ "{\"ok\":false,\"error\":\"unknown_tool\",\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\"}";

			log_->append( "tool.result", std::move( payload ) );

			return finish( );
		}

		const auto found = handlers_.find( call.name );
		const auto* handler = found != handlers_.end( ) ? &found->second : nullptr;

		if ( handler == nullptr ) {
			outcome.ok = false;
			outcome.code = errc::tool_failed;
			outcome.error_message = "tool has no handler registered: " + call.name;
			log_->append( "tool.result", "{\"ok\":false,\"error\":\"no_handler\"}" );

			return finish( );
		}

		auto produced = result< std::string >{ std::unexpected( fail( errc::tool_failed,
			"tool handler threw" ) ) };

		try {
			produced = ( *handler )( call.args_json );
		} catch ( const std::exception& error ) {
			produced = std::unexpected( fail( errc::tool_failed,
				std::string{ "tool handler threw: " } + error.what( ) ) );
		} catch ( ... ) {
			produced = std::unexpected( fail( errc::tool_failed,
				"tool handler threw a non-standard exception" ) );
		}

		budget_.charge( 0, 0.0 );

		if ( !produced ) {
			outcome.ok = false;
			outcome.code = produced.error( ).code;
			outcome.error_message = produced.error( ).msg;

			auto payload = std::string{ "{\"ok\":false,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"error\":\"";
			json::append_escaped( payload, produced.error( ).msg );
			payload += "\"}";

			log_->append( "tool.result", std::move( payload ) );

			return finish( );
		}

		outcome.ok = true;
		outcome.content = *produced;

		{
			auto payload = std::string{ "{\"ok\":true,\"tool\":\"" };
			json::append_escaped( payload, call.name );
			payload += "\",\"source\":\"";
			payload += to_string( definition->source );
			payload += "\"}";

			log_->append( "tool.result", std::move( payload ) );
		}

		return finish( );
	}

} // namespace mcode
