#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/fs/workspace.hxx"

namespace mcode::perm {

	inline constexpr std::string_view DENIED_FLOOR_MESSAGE =
		"refused by the hard-deny floor, which no flag or config can override";

	enum class permission_decision {
		allow,
		ask,
		deny,
	};

	[[nodiscard]] constexpr auto to_string( const permission_decision value ) noexcept
		-> std::string_view {
		switch ( value ) {
			case permission_decision::allow: return "allow";
			case permission_decision::ask: return "ask";
			case permission_decision::deny: return "deny";
		}

		return "ask";
	}

	// What the engine is asked about. The class comes from the registry, so an
	// extension tool and a built-in with the same class are treated identically.
	struct permission_request {
		std::string tool_name;
		tool_class klass = tool_class::read;
		std::string owner;
		std::string resource;
	};

	// One matched rule, for the `[?]` detail view and the model-facing error.
	struct rule_match {
		std::string scope;
		std::string pattern;
		permission_decision decision = permission_decision::ask;
	};

	// How the engine resolves an `ask` once the user has answered.
	enum class resolution {
		allowed_once,
		allowed_session,
		allowed_remembered,
		denied_once,
		denied_session,
		denied_remembered,
		denied_headless,
		denied_refused,
	};

	[[nodiscard]] auto to_string( const resolution value ) -> std::string_view;

	// The engine's own record of one decision, for tests and the detail view.
	struct permission_verdict {
		permission_decision decision = permission_decision::deny;
		rule_match matched;
		std::string reason;
	};

	// A rule is (class, pattern) -> decision. Exec patterns match the canonical
	// argv string exactly -- never a prefix. Path patterns are globs over the
	// canonical path. Tool patterns match the tool name exactly.
	struct rule {
		tool_class klass = tool_class::exec;
		std::string pattern;
		permission_decision decision = permission_decision::ask;
	};

	// Where a rule came from. Higher scopes cannot be overridden by lower ones
	// for the same decision class; evaluation is deny -> ask -> allow across the
	// merged list, and the list is ordered managed > user > project > session.
	enum class rule_scope {
		managed,
		user,
		project,
		session,
	};

	[[nodiscard]] constexpr auto to_string( const rule_scope value ) noexcept
		-> std::string_view {
		switch ( value ) {
			case rule_scope::managed: return "managed";
			case rule_scope::user: return "user";
			case rule_scope::project: return "project";
			case rule_scope::session: return "session";
		}

		return "user";
	}

	// The permission engine. One vocabulary (`permission_decision`), one check
	// point per tool call, and the hard-deny floor as a separate check ahead of
	// the rule merge and ahead of yolo.
	//
	// The engine is constructed with the workspace and the remember store; the
	// approval source is set per run, because headless and interactive runs
	// differ. Construction loads nothing from disk: `load_stores` does that,
	// so a caller can attach rules from config first.
	class permission_engine {
	public:
		struct options {
			bool yolo = false;
			bool headless = false;
			// `always` prompts even for reads; `never` and `on-request` only
			// prompt where the rules say ask. yolo implies never.
			std::string approval = "on-request";
		};

		permission_engine( const mcode::workspace& space, remember_store* store );

		// The approval source. Terminal in an interactive run, headless under
		// --json, scripted in tests. Set once before the first decision.
		auto set_approval_source( approval_source* source ) -> void;
		auto set_options( const options& value ) -> void;

		// Config-scope rules, from the merged `[permissions]` keys. Project
		// scope can only add ask/deny; that was enforced at merge, so anything
		// arriving here is honoured as written.
		auto add_config_rules( const rule_scope scope,
			const std::vector< std::string >& deny,
			const std::vector< std::string >& ask ) -> void;

		// Loads the remember store, dropping project-scope allows with a
		// warning. Called once at startup; the engine holds the entries in
		// memory and the store is re-read on every save.
		[[nodiscard]] auto load_store( ) -> status;

		// Loads a second layer over the first, for the repository's own
		// `.mcode/permissions.json`. Its allows are dropped when the file sits
		// under the workspace root, so a cloned repository cannot grant itself
		// permissions; denies survive. Answers the user gives with `[a]` are
		// written to the primary store, never to this one.
		[[nodiscard]] auto load_project_store( remember_store& store ) -> status;

		// Adds extra workspace roots (--add-dir). Widens the boundary for one
		// run; paths under an added root are inside for every check.
		auto add_root( const std::filesystem::path& root ) -> void;

		// The one entry point. Resolves prompts through the approval source,
		// records session rules and remember-store entries, and returns the
		// final decision. Never throws.
		[[nodiscard]] auto decide( const permission_request& request )
			-> permission_decision;

		// The verdict behind the last `decide` on this thread of calls: the
		// matched rule, the floor reason, or the prompt resolution. The `[?]`
		// view and the model-facing error both render from it.
		[[nodiscard]] auto last_verdict( ) const -> const permission_verdict& {
			return last_verdict_;
		}

		// Session-scope state, for tests and the detail view.
		[[nodiscard]] auto session_rule_count( ) const noexcept -> std::size_t {
			return session_rules_.size( );
		}

		// Load-time warnings: project-store allow drops and unreadable stores.
		[[nodiscard]] auto warnings( ) const noexcept -> const std::vector< std::string >& {
			return warnings_;
		}

		// The detail view for the `[?]` answer: full request, cwd, and the
		// rule that matched.
		[[nodiscard]] auto detail_view( const permission_request& request ) const
			-> std::string;

		// True when the request is on the hard-deny floor. Public so the file
		// tools can consult the .git/ rule without a full decide, and so the
		// floor tests can call it directly.
		[[nodiscard]] auto on_floor( const permission_request& request ) const
			-> std::optional< std::string >;

		// True when a path is inside the workspace or any added root.
		[[nodiscard]] auto inside_boundary( const std::filesystem::path& absolute ) const
			-> bool;

	private:
		// The default rule set, evaluated after the config/store/session rules.
		[[nodiscard]] auto default_decision( const permission_request& request ) const
			-> permission_verdict;

		// Rule evaluation over one list: deny -> ask -> allow, first match wins.
		[[nodiscard]] auto evaluate_rules( const permission_request& request ) const
			-> std::optional< rule_match >;

		[[nodiscard]] auto rule_matches( const rule& candidate,
			const permission_request& request ) const -> bool;

		// Resolves an `ask` through the approval source, applying yolo and
		// headless first. Mutates session rules and the store.
		[[nodiscard]] auto resolve_ask( const permission_request& request,
			const rule_match& matched ) -> permission_decision;

		const mcode::workspace* space_ = nullptr;
		remember_store* store_ = nullptr;
		approval_source* approval_ = nullptr;
		options options_;

		std::vector< std::pair< rule_scope, rule > > config_rules_;
		std::vector< rule > session_rules_;
		store_layer stored_;
		std::vector< std::filesystem::path > extra_roots_;

		// Load-time warnings (project-store drops, unreadable stores). Bounded
		// by the number of stores loaded, not by calls.
		std::vector< std::string > warnings_;

		permission_verdict last_verdict_;
	};

}

