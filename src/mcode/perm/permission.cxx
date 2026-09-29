#include "mcode/perm/permission.hxx"

#include "mcode/perm/argv.hxx"
#include "mcode/perm/floor.hxx"
#include "mcode/perm/rules.hxx"

#include <array>
#include <cstdlib>
#include <utility>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"

namespace mcode::perm {

	auto to_string( const resolution value ) -> std::string_view {
		switch ( value ) {
			case resolution::allowed_once: return "allowed once";
			case resolution::allowed_session: return "allowed for the session";
			case resolution::allowed_remembered: return "allowed (remembered)";
			case resolution::denied_once: return "denied once";
			case resolution::denied_session: return "denied for the session";
			case resolution::denied_remembered: return "denied (remembered)";
			case resolution::denied_headless: return "denied: headless run cannot ask";
			case resolution::denied_refused: return "denied: no answer was given";
		}

		return "denied";
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
		// A config rule names a pattern, not a class: `permissions.deny = [
		// "C:/secrets/*" ]` must deny reads AND writes of that path, and
		// `permissions.deny = [ "sudo rm -rf /" ]` must deny the exec call.
		// One pattern, one rule per class the matcher can distinguish; each
		// class's matcher decides whether the pattern fits its resource.
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

			auto root_entry = root.begin( );
			auto path_entry = canonical.begin( );
			auto inside = true;

			for ( ; root_entry != root.end( ); ++root_entry, ++path_entry ) {
				if ( path_entry == canonical.end( ) || *root_entry != *path_entry ) {
					inside = false;

					break;
				}
			}

			if ( inside ) {
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
		// deny -> ask -> allow, first match wins across the merged list. The
		// list is ordered managed > user > project > session by construction,
		// and each pass scans it in that order.
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

			// The store's entries join the evaluation as remembered answers.
			// The key comes from the same function that wrote them, so a
			// remembered answer is found by the same identity that stored it.
			// The store holds no ask entries, so only deny and allow participate.
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
			}
		}

		return std::nullopt;
	}

	auto permission_engine::default_decision( const permission_request& request ) const
		-> permission_verdict {
		// The default rule set from the resolved table. Workspace-internal
		// writes are allow -- the deliberate, recorded deviation from the
		// original table. Prompts are for commands and for writes outside the
		// workspace. `approval = always` prompts for everything, including
		// reads, which is the setting's entire purpose.
		switch ( request.klass ) {
			case tool_class::read: {
				auto candidate = std::filesystem::path{ request.resource };

				if ( candidate.is_relative( ) ) {
					candidate = space_->root( ) / candidate;
				}

				// A deny is not a question, so the default-set secret deny
				// holds even under `approval = always`: the paranoid setting
				// prompts for everything it may allow, never for a refusal.
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
		// yolo and approval = "never" disable prompts, not policy: every ask
		// resolves to allow whatever it is, but the floor and permissions.deny
		// already decided before this point.
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

		// The detail view renders from the verdict, so it must describe the
		// pending ask before the source can request it.
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
				// Persist the canonical key, never a prefix. A remembered
				// `git push` must not authorize `git push --force`. A write
				// outside the workspace is never auto-persisted -- the table
				// marks that row "no" -- and `store_key_for` returns nothing
				// for it.
				const auto key = store_key_for( { request.klass, request.tool_name,
					request.resource } );

				if ( store_ != nullptr && key ) {
					auto additions = store_layer{ };
					section_of( additions, key->section ).insert_or_assign( key->key,
						store_decision::allow );

					const auto saved = store_->save( additions );

					// The in-memory layer must reflect the answer either way, or
					// the second identical call in the same session prompts
					// again -- the failure the remember store exists to prevent.
					section_of( stored_, key->section ).insert_or_assign( key->key,
						store_decision::allow );

					if ( !saved ) {
						// A failed save is not a failed approval: the session
						// rule below still applies, and the reason is visible in
						// the verdict.
						last_verdict_ = { permission_decision::allow,
							{ "prompt", matched.pattern, permission_decision::allow },
							"allowed for the session; the store write failed: " +
								saved.error( ).msg };

						session_rules_.push_back( rule{ request.klass, key->key,
							permission_decision::allow } );

						return permission_decision::allow;
					}
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
				// A session-scope deny, never auto-persisted: a permanent deny
				// written on one keystroke is the decision a user regrets.
				// `permissions.deny` in a config file is how a permanent deny
				// is expressed.
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
				// The terminal source handles `?` internally and never returns
				// it; reaching here means a source misused the protocol.
				last_verdict_ = { permission_decision::deny,
					{ matched.scope, matched.pattern, permission_decision::deny },
					"approval source returned detail; failing closed" };

				return permission_decision::deny;
		}

		return permission_decision::deny;
	}

	auto permission_engine::decide( const permission_request& request )
		-> permission_decision {
		// 1. The hard-deny floor: ahead of the rule merge and ahead of yolo.
		//    Not a rule in the list -- a rule can be overridden by a later
		//    scope and the floor must not be.
		if ( const auto floor = on_floor( request ) ) {
			last_verdict_ = { permission_decision::deny,
				{ "floor", request.resource, permission_decision::deny }, *floor };

			return permission_decision::deny;
		}

		// 2. An unparsable or compound command arrives as an empty resource.
		//    No rule may match it -- an allow rule for "" would otherwise
		//    authorize every unparsable command -- and the default set asks.
		if ( request.klass == tool_class::exec && request.resource.empty( ) ) {
			const rule_match unparsable{ "default", "unparsable or compound command",
				permission_decision::ask };

			last_verdict_ = { permission_decision::ask, unparsable,
				"an unparsable or compound command is never auto-allowed" };

			return resolve_ask( request, unparsable );
		}

		// 3. An exec through a shell or runner wrapper is refused outright:
		//    the wrapper being allowlisted says nothing about what it runs,
		//    and a prefix rule against it is documented-bypassable.
		if ( request.klass == tool_class::exec ) {
			const auto tokens = parse_command_line( request.resource );

			if ( tokens && is_exec_runner( tokens->front( ) ) ) {
				const rule_match runner{ "engine", tokens->front( ),
					permission_decision::deny };

				last_verdict_ = { permission_decision::deny, runner,
					"commands through " + tokens->front( ) +
						" are never allowlisted; run the program directly" };

				return permission_decision::deny;
			}
		}

		// 4. The merged rule list: config scopes, session rules, the store.
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

			// An ask rule: resolve through the approval path.
			return resolve_ask( request, *matched );
		}

		// 5. The default set.
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

