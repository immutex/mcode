#include "mcode/cli/extension_wiring.hxx"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/cli/active_loop.hxx"
#include "mcode/cli/fetch_runner.hxx"
#include "mcode/cli/session_state.hxx"
#include "mcode/cli/web_search.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::cli {

	auto extension_collaborators::apply( mcode::ext::register_context& context ) const -> void {
		context.session_state = &session_state;
		context.web_searcher = &web_searcher;
		context.subagent_runner = &subagent_runner;
		context.bash_runner = &bash_runner;
		context.plan_sink = &plan_sink;
		context.fetch_runner = &fetch_runner;
	}

	auto build_collaborators( const collaborator_sources& sources )
		-> extension_collaborators {
		auto out = extension_collaborators{ };

		if ( sources.log != nullptr && sources.budget != nullptr ) {
			out.session_state = mcode::cli::session_state_provider( *sources.log,
				*sources.budget );
		}

		if ( sources.config != nullptr && sources.transport != nullptr ) {
			// Null when `[web] search_url` is unset, so `web_search` is absent from the
			// tool list rather than present and failing.
			out.web_searcher = mcode::cli::make_web_searcher( *sources.config,
				*sources.transport );

			// Null when `[web] fetch = false`. The SSRF filter and the per-domain gate
			// are the harness's, so they hold whatever the extension does with the
			// result.
			out.fetch_runner = mcode::cli::make_fetch_runner( *sources.config,
				*sources.transport, sources.permissions );
		}

		// The child is built from the running loop's own collaborators, so an
		// extension cannot substitute a client, a workspace or a permission engine.
		out.subagent_runner = [ ]( const mcode::subagent_spec& spec )
			-> mcode::result< mcode::subagent_report > {
			auto* loop = mcode::cli::active_loop( );

			if ( loop == nullptr ) {
				return std::unexpected( mcode::fail( mcode::errc::config,
					"no running loop to spawn a subagent from" ) );
			}

			return loop->spawn_subagent( spec );
		};

		// The same core handler the model's `bash` tool calls, so the sandbox profile
		// and the permission gate are shared rather than reimplemented.
		if ( sources.tools != nullptr ) {
			out.bash_runner = [ tools = sources.tools ]( const std::string& command,
				const std::int64_t timeout_ms ) -> mcode::result< std::string > {
				auto payload = std::string{ "{\"command\":\"" };
				mcode::json::append_escaped( payload, command );
				payload += "\",\"timeout_ms\":";
				payload += std::to_string( timeout_ms );
				payload += '}';

				const auto parsed = mcode::tools::tool_args::parse( payload );

				if ( !parsed ) {
					return std::unexpected( parsed.error( ) );
				}

				return mcode::tools::handle_bash( *parsed, *tools );
			};
		}

		// The plan is the loop's state, written through the active loop so recitation
		// picks it up at the next refresh whatever the `todo` extension does.
		out.plan_sink = [ ]( const std::string_view plan ) {
			if ( auto* loop = mcode::cli::active_loop( ); loop != nullptr ) {
				loop->set_plan( std::string{ plan } );
			}
		};

		return out;
	}

}
