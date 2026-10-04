#include "mcode/agent/loop.hxx"

#include "mcode/agent/loop_internal.hxx"

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

		for ( const auto& value : options.history ) {
			out.request.messages.push_back( value );
		}

		if ( !options.recitation.empty( ) ) {
			auto tail = model::message{ };
			tail.speaker = model::role::user;

			auto tail_block = model::block{ };
			tail_block.kind = model::block_kind::text;
			tail_block.text = std::string{ options.recitation };
			tail.blocks.push_back( std::move( tail_block ) );

			out.request.messages.push_back( std::move( tail ) );
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

	// compaction keeps the first events and the newest tail, dropping everything between them.
	auto agent_loop::maybe_compact( ) -> status {
		const auto window_tokens = caps_.effective_context_window( );
		const auto usable = static_cast< double >( window_tokens - RESERVED_OUTPUT_TOKENS );
		const auto window = usable * ( 1.0 - SAFETY_MARGIN_FRACTION );
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

		auto result = compaction_result{ };

		result.pinned_facts.push_back( user_task_ );

		for ( auto index = std::size_t{ 0 }; index < pinned; ++index ) {
			result.kept.push_back( history_[ index ] );
		}

		for ( auto index = tail_start; index < history_.size( ); ++index ) {
			result.kept.push_back( history_[ index ] );
		}

		history_ = std::move( result.kept );

		auto payload = std::string{ "{\"reason\":\"80pct\",\"kept_tokens\":" };
		payload += std::to_string( kept_tokens );
		payload += "}";

		log_->append( "context.compaction", payload );
		publish( events::kind::compaction, std::move( payload ) );

		return status{ };
	}

	auto build_system_prompt( const tool_registry& registry,
		const std::string_view instruction_chain,
		const std::string_view skill_index ) -> std::string {
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

		return out;
	}


} // namespace mcode
