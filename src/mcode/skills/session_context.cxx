#include "mcode/skills/session_context.hxx"

#include "mcode/skills/index.hxx"

namespace mcode::skills {

	auto assemble_session_context( const session_context_options& options )
		-> session_context {
		auto context = session_context{ };

		auto discovery = discovery_options{ };
		discovery.workspace = options.workspace;
		discovery.user_root = options.user_skills_root;
		discovery.extension_roots = options.extension_roots;

		context.skills = discover_skills( discovery );

		auto chain = mcode::instruct::chain_options{ };
		chain.start = options.workspace;
		chain.user_file = options.user_agents_file;
		chain.org_file = options.org_agents_file;

		context.chain = mcode::instruct::assemble_chain( chain );
		context.skill_index = render_index( context.skills.entries );

		context.warnings = context.chain.warnings;

		return context;
	}

}
