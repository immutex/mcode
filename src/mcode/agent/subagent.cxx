#include "mcode/agent/subagent.hxx"

#include <algorithm>
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

	auto run_subagent( const subagent_request& request ) -> result< subagent_result > {
		if ( request.parent == nullptr ) {
			return std::unexpected( fail( errc::config, "a subagent needs a parent to borrow from" ) );
		}

		auto registry = read_only_registry( *request.parent->registry );

		if ( !registry ) {
			return std::unexpected( registry.error( ) );
		}

		auto deps = *request.parent;
		deps.registry = &*registry;

		auto budget = session_budget{ };
		budget.max_steps = static_cast< std::uint32_t >( request.max_steps );
		deps.budget = budget;

		// Not the parent's history or instruction chain: the spawn payload is the only context.
		deps.instruction_chain = { };
		deps.skill_index = { };
		deps.snapshots = nullptr;

		auto loop = agent_loop{ deps };

		for ( const auto& [ name, handler ] : request.handlers ) {
			if ( is_read_only( name ) ) {
				loop.register_handler( name, handler );
			}
		}

		auto task = std::string{ request.objective };

		if ( !request.context.empty( ) ) {
			task += "\n\nContext you need, supplied by the caller:\n";
			task += request.context;
		}

		const auto outcome = loop.run( task );

		if ( !outcome ) {
			return std::unexpected( outcome.error( ) );
		}

		auto result = subagent_result{ };
		result.steps = static_cast< std::uint64_t >( outcome->steps );
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

}
