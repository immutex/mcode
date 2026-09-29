#include "mcode/agent/loop.hxx"

#include "mcode/agent/loop_internal.hxx"

#include <algorithm>
#include <map>

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
			out.near_budget_note = true;
		}

		out.prefix_bytes = out.request.messages.size( ) > 0 ? 1 : 0;

		return out;
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

		// Sections 10 and 11 are session-start only: both arrive fully rendered
		// from the caller, and nothing here recomputes per turn, so the cached
		// prefix stays byte-stable.
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
