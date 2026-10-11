#include "mcode/agent/subagent.hxx"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/support/text.hxx"

namespace mcode {

	namespace {

		// No shell: `python -c` writes files, which is how a "read-only" mode stops being one.
		const auto READ_ONLY_TOOLS = std::vector< std::string_view >{
			"read", "glob", "grep",
		};

		constexpr std::size_t MAX_SUMMARY_TOKENS = 2000;

		// A child's ceiling, so one subagent cannot consume the parent's whole run.
		constexpr std::uint64_t SUBAGENT_TOKEN_SHARE = 400'000;
		constexpr double SUBAGENT_USD_SHARE = 1.0;

		auto is_read_only( const std::string_view name ) -> bool {
			return std::find( READ_ONLY_TOOLS.begin( ), READ_ONLY_TOOLS.end( ), name )
				!= READ_ONLY_TOOLS.end( );
		}

		// Only the last assistant text: the trajectory is what the spawn discards.
		auto last_assistant_text( const std::vector< model::message >& history ) -> std::string {
			for ( auto index = history.size( ); index > 0; --index ) {
				const auto& message = history[ index - 1 ];

				if ( message.speaker != model::role::assistant ) {
					continue;
				}

				const auto text = message.text( );

				if ( !text.empty( ) ) {
					return text;
				}
			}

			return { };
		}

	}

	auto subagent_tool_names( ) -> const std::vector< std::string_view >& {
		return READ_ONLY_TOOLS;
	}

	auto read_only_registry( const tool_registry& parent ) -> result< tool_registry > {
		auto child = tool_registry{ };

		for ( const auto* definition : parent.all( ) ) {
			if ( !is_read_only( definition->name ) ) {
				continue;
			}

			// Fail closed: a skipped tool reads as a capability the child lacks.
			if ( const auto added = child.add( *definition ); !added ) {
				return std::unexpected( added.error( ) );
			}
		}

		return child;
	}

	auto subagent_system_prompt( ) -> std::string_view {
		return "You are a read-only research subagent. You have the read, glob and grep"
			" tools and nothing else: you cannot write, edit or run commands, and a call"
			" to one of those tools will be rejected. Investigate the question you were"
			" given and report what you found.\n\n"
			"Cite file paths and line numbers for every claim. If you could not establish"
			" something, say so explicitly under a heading -- a confident wrong answer is"
			" worse than a stated gap. Keep the report under 2000 tokens; anything larger"
			" belongs in a file you name by path.\n";
	}

	auto run_subagent( const subagent_request& request ) -> result< subagent_report > {
		if ( request.parent == nullptr ) {
			return std::unexpected( fail( errc::config, "a subagent needs a parent to borrow from" ) );
		}

		auto registry = read_only_registry( *request.parent->registry );

		if ( !registry ) {
			return std::unexpected( registry.error( ) );
		}

		auto deps = *request.parent;
		deps.registry = &*registry;

		// A child budget carved from the parent's remainder, not a fresh allowance:
		// a fresh one lets a fan-out spend without bound while the run reports a
		// healthy budget. The caps are the child's; the parent still owns the run.
		auto budget = session_budget{ };
		const auto& parent_budget = request.parent->budget;
		budget.max_steps = static_cast< std::uint32_t >( request.spec.max_steps );

		const auto parent_tokens_left = parent_budget.max_tokens > parent_budget.tokens_used
			? parent_budget.max_tokens - parent_budget.tokens_used : 0;
		budget.max_tokens = std::min( parent_tokens_left, SUBAGENT_TOKEN_SHARE );

		const auto parent_usd_left = parent_budget.max_usd > parent_budget.usd_used
			? parent_budget.max_usd - parent_budget.usd_used : 0.0;
		budget.max_usd = std::min( parent_usd_left, SUBAGENT_USD_SHARE );

		deps.budget = budget;

		// Not the parent's history or instruction chain: the spawn payload is the only context.
		deps.instruction_chain = { };
		deps.skill_index = { };
		deps.snapshots = nullptr;

		// The spawn is inside a parent step, so the child must not pump the parent's
		// timers: that is the same VM, re-entered from inside a running handler.
		deps.pump_timers = { };
		deps.pump_deferred = { };

		auto loop = agent_loop{ deps };

		for ( const auto& [ name, handler ] : request.handlers ) {
			if ( is_read_only( name ) ) {
				loop.register_handler( name, handler );
			}
		}

		auto task = std::string{ request.spec.objective };

		if ( !request.spec.context.empty( ) ) {
			task += "\n\nContext you need, supplied by the caller:\n";
			task += request.spec.context;
		}

		const auto outcome = loop.run( task );

		if ( !outcome ) {
			return std::unexpected( outcome.error( ) );
		}

		auto result = subagent_report{ };
		result.steps = static_cast< std::uint64_t >( outcome->steps );

		// Read from the loop, not the local `budget`: the loop owns its own copy, so
		// the local one is never charged.
		result.tokens_used = loop.budget( ).tokens_used.load( );
		result.usd_used = loop.budget( ).usd_used.load( );
		result.summary = last_assistant_text( loop.history( ) );

		if ( result.summary.empty( ) ) {
			// Cancelled or exhausted before it answered: a partial report beats none.
			result.truncated = true;
			result.summary = "the subagent stopped before producing a report";
		}

		const auto budget_tokens = static_cast< std::uint32_t >( MAX_SUMMARY_TOKENS )
			* static_cast< std::uint32_t >( text::CHARS_PER_TOKEN );

		if ( result.summary.size( ) > budget_tokens ) {
			result.truncated = true;
			result.summary = text::truncate( result.summary, budget_tokens,
				"\n[truncated: the report exceeded the return budget]" );
		}

		return result;
	}

	auto agent_loop::spawn_subagent( const subagent_spec& spec )
		-> result< subagent_report > {
		// Built from this loop's own collaborators rather than a caller's copy, so the
		// spawn is wired to the same workspace, client and permission engine the parent
		// runs against and an extension cannot substitute its own. The handlers come
		// too: without them the child's read-only registry has nothing behind it.
		auto handlers = std::vector< std::pair< std::string, tool_handler > >{ };

		for ( const auto& [ name, handler ] : handlers_ ) {
			if ( std::ranges::find( subagent_tool_names( ), name )
				!= subagent_tool_names( ).end( ) ) {
				handlers.emplace_back( name, handler );
			}
		}

		auto parent = dependencies{ };
		parent.registry = registry_;
		parent.client = client_;
		parent.log = log_;
		parent.bus = bus_;
		parent.budget = budget_;
		parent.model_name = model_name_;
		parent.caps = caps_;
		parent.provider = provider_;
		parent.api_key = api_key_;
		parent.workspace_root = workspace_root_;
		parent.platform_name = platform_name_;
		parent.permissions = permissions_;

		auto spawned = run_subagent( { .spec = spec, .parent = &parent,
			.handlers = std::move( handlers ) } );

		if ( !spawned ) {
			return std::unexpected( spawned.error( ) );
		}

		// The run is charged for what the child spent; the child's own step count is
		// not the parent's, so only its tokens and cost are folded back.
		budget_.absorb_child_spend( spawned->tokens_used, spawned->usd_used );

		return spawned;
	}

}
