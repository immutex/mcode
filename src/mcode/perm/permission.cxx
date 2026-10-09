#include "mcode/perm/permission.hxx"

#include "mcode/perm/argv.hxx"
#include "mcode/perm/floor.hxx"
#include "mcode/perm/rules.hxx"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <utility>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/glob.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"
#include "mcode/support/text.hxx"

namespace mcode::perm {

	namespace {


		// `find . -exec rm {} +` runs a program, so `find` is exec-capable when it carries `-exec`.
		[[nodiscard]] auto is_exec_capable( const std::vector< std::string >& raw_argv ) -> bool {
			const auto argv = unwrap_command( raw_argv );

			if ( argv.empty( ) ) {
				return false;
			}

			if ( is_exec_runner( argv.front( ) ) ) {
				return true;
			}

			const auto program = mcode::text::program_basename( argv.front( ) );

			if ( program != "find" && program != "find.exe" ) {
				return false;
			}

			for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
				if ( argv[ index ] == "-exec" || argv[ index ] == "-execdir" ) {
					return true;
				}
			}

			return false;
		}

		// plan mode is read-only: only the read class passes.
		[[nodiscard]] constexpr auto denied_by_plan_mode( const tool_class klass ) noexcept
			-> bool {
			switch ( klass ) {
				case tool_class::read: return false;

				case tool_class::write:
				case tool_class::exec:
				case tool_class::net:
				case tool_class::spawn:
				// mcp is not a read: an MCP `readOnlyHint` is an untrusted claim.
				case tool_class::mcp: return true;
			}

			// unreachable for a valid class; an unknown one fails closed like the default set.
			return true;
		}

	}

	permission_engine::permission_engine( const mcode::workspace& space,
		remember_store* store ) : space_( &space ), store_( store ) { }

	auto permission_engine::set_approval_source( approval_source* source ) -> void {
		approval_ = source;
	}

	auto permission_engine::set_options( const options& value ) -> void {
		options_ = value;
	}

	auto permission_engine::add_config_rules( const rule_scope scope,
		const std::vector< std::string >& deny, const std::vector< std::string >& ask )
		-> void {
		// a config rule names a pattern, not a class, so one pattern becomes one rule per class.
		for ( const auto klass : { tool_class::exec, tool_class::read,
			tool_class::write } ) {
			for ( const auto& pattern : deny ) {
				config_rules_.push_back( { scope, rule{ klass, pattern,
					permission_decision::deny } } );
			}

			for ( const auto& pattern : ask ) {
				config_rules_.push_back( { scope, rule{ klass, pattern,
					permission_decision::ask } } );
			}
		}
	}

	auto permission_engine::add_root( const std::filesystem::path& root ) -> void {
		auto canonical = platform::canonicalize( root );

		if ( canonical ) {
			extra_roots_.push_back( *canonical );
		}
	}

	auto permission_engine::inside_boundary( const std::filesystem::path& absolute ) const
		-> bool {
		if ( space_->contains( absolute ) ) {
			return true;
		}

		for ( const auto& root : extra_roots_ ) {
			auto error_code = std::error_code{ };
			const auto canonical = std::filesystem::weakly_canonical( absolute, error_code );

			if ( error_code ) {
				continue;
			}

			if ( mcode::path_is_within( root, canonical ) ) {
				return true;
			}
		}

		return false;
	}

	auto permission_engine::on_floor( const permission_request& request ) const
		-> std::optional< std::string > {
		return floor_reason( { request.klass, request.tool_name, request.resource },
			*space_ );
	}

	auto permission_engine::rule_matches( const rule& candidate,
		const permission_request& request ) const -> bool {
		return perm::rule_matches( candidate, request.klass, request.resource,
			request.tool_name );
	}

	auto permission_engine::evaluate_rules( const permission_request& request ) const
		-> std::optional< rule_match > {
		// deny -> ask -> allow, first match wins; each pass scans the merged list in scope order.
		for ( const auto decision : { permission_decision::deny,
			permission_decision::ask, permission_decision::allow } ) {
			for ( const auto& [ scope, candidate ] : config_rules_ ) {
				if ( candidate.decision != decision ) {
					continue;
				}

				if ( rule_matches( candidate, request ) ) {
					return rule_match{ std::string{ to_string( scope ) }, candidate.pattern,
						decision };
				}
			}

			for ( const auto& candidate : session_rules_ ) {
				if ( candidate.decision != decision ) {
					continue;
				}

				if ( rule_matches( candidate, request ) ) {
					return rule_match{ std::string{ to_string( rule_scope::session ) },
						candidate.pattern, decision };
				}
			}

			// the store holds no ask entries, so only deny and allow participate.
			if ( decision != permission_decision::ask ) {
				const auto key = store_key_for( { request.klass, request.tool_name,
					request.resource } );

				const auto wanted = decision == permission_decision::deny
					? store_decision::deny
					: store_decision::allow;

				if ( key ) {
					const auto& entries = section_of( stored_, key->section );
					const auto found = entries.find( key->key );

					if ( found != entries.end( ) && found->second == wanted ) {
						return rule_match{ "store", key->key, decision };
					}
				}

				// `paths` is hand-written globs, so it has no store key and is matched here.
				if ( request.klass == tool_class::read || request.klass == tool_class::write ) {
					for ( const auto& [ pattern, entry ] : stored_.paths ) {
						if ( entry != wanted ) {
							continue;
						}

						if ( support::glob_match( pattern, request.resource ) ) {
							return rule_match{ "store", pattern, decision };
						}
					}
				}
			}
		}

		return std::nullopt;
	}

	auto permission_engine::default_decision( const permission_request& request ) const
		-> permission_verdict {
		switch ( request.klass ) {
			case tool_class::read: {
				auto candidate = std::filesystem::path{ request.resource };

				if ( candidate.is_relative( ) ) {
					candidate = space_->root( ) / candidate;
				}

				// a deny is not a question: the secret deny holds even under approval = always.
				if ( const auto secret = default_secret_deny( candidate.generic_string( ) ) ) {
					return { permission_decision::deny,
						{ "default", *secret, permission_decision::deny },
						"reading " + *secret + " is denied by the default set" };
				}

				if ( options_.approval == "always" ) {
					return { permission_decision::ask,
						{ "default", "approval = always", permission_decision::ask },
						"approval = always prompts for every tool" };
				}

				if ( !inside_boundary( candidate ) ) {
					return { permission_decision::ask,
						{ "default", "read outside the workspace", permission_decision::ask },
						"reads outside the workspace prompt" };
				}

				return { permission_decision::allow,
					{ "default", "read inside the workspace", permission_decision::allow }, { } };
			}

			case tool_class::write: {
				if ( options_.approval == "always" ) {
					return { permission_decision::ask,
						{ "default", "approval = always", permission_decision::ask },
						"approval = always prompts for every tool" };
				}

				auto candidate = std::filesystem::path{ request.resource };

				if ( candidate.is_relative( ) ) {
					candidate = space_->root( ) / candidate;
				}

				if ( inside_boundary( candidate ) ) {
					return { permission_decision::allow,
						{ "default", "write inside the workspace", permission_decision::allow },
						{ } };
				}

				return { permission_decision::ask,
					{ "default", "write outside the workspace", permission_decision::ask },
					"writes outside the workspace prompt" };
			}

			case tool_class::exec:
			case tool_class::net:
			case tool_class::spawn:
			case tool_class::mcp: {
				return { permission_decision::ask,
					{ "default", std::string{ to_string( request.klass ) },
						permission_decision::ask },
					"commands prompt per exact argv" };
			}
		}

		return { permission_decision::deny, { "default", "unknown class",
			permission_decision::deny }, "unknown tool class fails closed" };
	}

	auto permission_engine::resolve_ask( const permission_request& request,
		const rule_match& matched ) -> permission_decision {
		// yolo disables prompts, not policy: the floor and permissions.deny decided before here.
		if ( options_.yolo || options_.approval == "never" ) {
			last_verdict_ = { permission_decision::allow,
				{ "yolo", matched.pattern, permission_decision::allow },
				"yolo or approval = never resolves the prompt to allow" };

			return permission_decision::allow;
		}

		if ( options_.headless || approval_ == nullptr ) {
			last_verdict_ = { permission_decision::deny,
				{ matched.scope, matched.pattern, permission_decision::deny },
				"headless run cannot ask; the call is denied" };

			return permission_decision::deny;
		}

		auto ask_request = approval_request{ };
		ask_request.action = request.klass == tool_class::exec
			? std::string{ "run" }
			: std::string{ "call " } + request.tool_name;
		ask_request.subject = request.resource;
		ask_request.rule = matched.scope + ": " + matched.pattern;
		ask_request.working_directory = space_->root( ).string( );

		// the detail view renders from the verdict, so it is set before the source asks.
		last_verdict_ = { permission_decision::ask, matched,
			"the rule matched and the user is being asked" };

		const auto outcome = approval_->ask( ask_request,
			[ & ]( ) { return detail_view( request ); } );

		switch ( outcome ) {
			case approval_outcome::allow_once:
				last_verdict_ = { permission_decision::allow,
					{ "prompt", matched.pattern, permission_decision::allow },
					"allowed once by the user" };

				return permission_decision::allow;

			case approval_outcome::allow_remember: {
				// a write outside the workspace is never auto-persisted.
				const auto key = store_key_for( { request.klass, request.tool_name,
					request.resource } );

				// no key means nothing can be persisted, so the answer stays session-scoped.
				if ( store_ == nullptr || !key ) {
					session_rules_.push_back( rule{ request.klass, request.resource,
						permission_decision::allow } );

					last_verdict_ = { permission_decision::allow,
						{ "session", request.resource, permission_decision::allow },
						"allowed for the session; this request has no persisted form" };

					return permission_decision::allow;
				}

				auto additions = store_layer{ };
				section_of( additions, key->section ).insert_or_assign( key->key,
					store_decision::allow );

				const auto saved = store_->save( additions );

				// the in-memory layer must reflect the answer either way.
				section_of( stored_, key->section ).insert_or_assign( key->key,
					store_decision::allow );

				if ( !saved ) {
					// a failed save is not a failed approval; the session rule still applies.
					last_verdict_ = { permission_decision::allow,
						{ "prompt", matched.pattern, permission_decision::allow },
						"allowed for the session; the store write failed: " +
							saved.error( ).msg };

					session_rules_.push_back( rule{ request.klass, key->key,
						permission_decision::allow } );

					return permission_decision::allow;
				}

				last_verdict_ = { permission_decision::allow,
					{ "prompt", matched.pattern, permission_decision::allow },
					"allowed and remembered" };

				return permission_decision::allow;
			}

			case approval_outcome::deny_once:
				last_verdict_ = { permission_decision::deny,
					{ "prompt", matched.pattern, permission_decision::deny },
					"denied once by the user" };

				return permission_decision::deny;

			case approval_outcome::deny_session:
				// a permanent deny written on one keystroke is the decision a user regrets.
				session_rules_.push_back( rule{ request.klass, request.resource,
					permission_decision::deny } );

				last_verdict_ = { permission_decision::deny,
					{ "session", request.resource, permission_decision::deny },
					"denied for the session by the user" };

				return permission_decision::deny;

			case approval_outcome::refused:
				last_verdict_ = { permission_decision::deny,
					{ matched.scope, matched.pattern, permission_decision::deny },
					"no answer was given; failing closed" };

				return permission_decision::deny;

			case approval_outcome::detail:
				// the terminal source handles `?` internally and never returns it.
				last_verdict_ = { permission_decision::deny,
					{ matched.scope, matched.pattern, permission_decision::deny },
					"approval source returned detail; failing closed" };

				return permission_decision::deny;
		}

		return permission_decision::deny;
	}

	auto permission_engine::decide( const permission_request& request )
		-> permission_decision {
		// plan mode is read-only, and decides ahead of the rule merge and of yolo.
		if ( options_.plan_mode && denied_by_plan_mode( request.klass ) ) {
			last_verdict_ = { permission_decision::deny,
				{ "plan", request.resource, permission_decision::deny },
				"plan mode is read-only: it refuses edits and commands" };

			return permission_decision::deny;
		}

		// the hard-deny floor sits ahead of the rule merge and ahead of yolo.
		if ( const auto floor = on_floor( request ) ) {
			last_verdict_ = { permission_decision::deny,
				{ "floor", request.resource, permission_decision::deny }, *floor };

			return permission_decision::deny;
		}

		// an unparsable command arrives as an empty resource, which no rule may match.
		if ( request.klass == tool_class::exec && request.resource.empty( ) ) {
			const rule_match unparsable{ "default", "unparsable or compound command",
				permission_decision::ask };

			last_verdict_ = { permission_decision::ask, unparsable,
				"an unparsable or compound command is never auto-allowed" };

			return resolve_ask( request, unparsable );
		}

		if ( request.klass == tool_class::exec ) {
			const auto tokens = parse_command_line( request.resource );

			// a leading wrapper hides the runner, so the check runs on the stripped argv.
			if ( tokens && is_exec_capable( *tokens ) ) {
				const auto runner_name = unwrap_command( *tokens ).front( );
				const rule_match runner{ "engine", runner_name,
					permission_decision::deny };

				last_verdict_ = { permission_decision::deny, runner,
					"commands through " + runner_name +
						" are never allowlisted; run the program directly" };

				return permission_decision::deny;
			}
		}

		if ( const auto matched = evaluate_rules( request ) ) {
			if ( matched->decision == permission_decision::deny ) {
				last_verdict_ = { permission_decision::deny, *matched,
					"denied by " + matched->scope + " rule '" + matched->pattern + "'" };

				return permission_decision::deny;
			}

			if ( matched->decision == permission_decision::allow ) {
				last_verdict_ = { permission_decision::allow, *matched,
					"allowed by " + matched->scope + " rule '" + matched->pattern + "'" };

				return permission_decision::allow;
			}

			return resolve_ask( request, *matched );
		}

		const auto fallback = default_decision( request );

		if ( fallback.decision != permission_decision::ask ) {
			last_verdict_ = fallback;

			return fallback.decision;
		}

		return resolve_ask( request, fallback.matched );
	}

	auto permission_engine::detail_view( const permission_request& request ) const
		-> std::string {
		auto out = std::string{ "  tool:   " };
		out += request.tool_name;
		out += "\n  class:  ";
		out += to_string( request.klass );
		out += "\n  subject: ";
		out += request.resource;
		out += "\n  cwd:    ";
		out += space_->root( ).string( );

		if ( !last_verdict_.matched.scope.empty( ) ) {
			out += "\n  rule:   ";
			out += last_verdict_.matched.scope;
			out += ": ";
			out += last_verdict_.matched.pattern;
			out += " -> ";
			out += to_string( last_verdict_.matched.decision );
		}

		if ( !last_verdict_.reason.empty( ) ) {
			out += "\n  reason: ";
			out += last_verdict_.reason;
		}

		return out;
	}

	auto permission_engine::load_store( ) -> status {
		return load_store_into( store_, *space_, stored_, warnings_ );
	}

	auto permission_engine::load_project_store( remember_store& store ) -> status {
		return load_store_into( &store, *space_, stored_, warnings_ );
	}

}

