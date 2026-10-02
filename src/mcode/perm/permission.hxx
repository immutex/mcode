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

	struct permission_request {
		std::string tool_name;
		tool_class klass = tool_class::read;
		std::string owner;
		std::string resource;
	};

	struct rule_match {
		std::string scope;
		std::string pattern;
		permission_decision decision = permission_decision::ask;
	};

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

	struct permission_verdict {
		permission_decision decision = permission_decision::deny;
		rule_match matched;
		std::string reason;
	};

	// exec patterns match the canonical argv exactly; read/write patterns glob the path.
	struct rule {
		tool_class klass = tool_class::exec;
		std::string pattern;
		permission_decision decision = permission_decision::ask;
	};

	// deny -> ask -> allow across the merged list, ordered managed > user > project > session.
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

	class permission_engine {
	public:
		struct options {
			bool yolo = false;
			bool headless = false;
			// `always` prompts even for reads; yolo implies never.
			std::string approval = "on-request";
		};

		permission_engine( const mcode::workspace& space, remember_store* store );

		auto set_approval_source( approval_source* source ) -> void;
		auto set_options( const options& value ) -> void;

		// project scope can only add ask/deny, enforced at merge, so this is honoured as written.
		auto add_config_rules( const rule_scope scope,
			const std::vector< std::string >& deny,
			const std::vector< std::string >& ask ) -> void;

		[[nodiscard]] auto load_store( ) -> status;

		// the repository's own `.mcode/permissions.json`; a clone's allows are dropped.
		[[nodiscard]] auto load_project_store( remember_store& store ) -> status;

		auto add_root( const std::filesystem::path& root ) -> void;

		[[nodiscard]] auto decide( const permission_request& request )
			-> permission_decision;

		[[nodiscard]] auto last_verdict( ) const -> const permission_verdict& {
			return last_verdict_;
		}

		[[nodiscard]] auto warnings( ) const noexcept -> const std::vector< std::string >& {
			return warnings_;
		}

		[[nodiscard]] auto detail_view( const permission_request& request ) const
			-> std::string;

		// public so the file tools can consult the .git/ rule without a full decide.
		[[nodiscard]] auto on_floor( const permission_request& request ) const
			-> std::optional< std::string >;

		[[nodiscard]] auto inside_boundary( const std::filesystem::path& absolute ) const
			-> bool;

	private:
		[[nodiscard]] auto default_decision( const permission_request& request ) const
			-> permission_verdict;

		[[nodiscard]] auto evaluate_rules( const permission_request& request ) const
			-> std::optional< rule_match >;

		[[nodiscard]] auto rule_matches( const rule& candidate,
			const permission_request& request ) const -> bool;

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

		std::vector< std::string > warnings_;

		permission_verdict last_verdict_;
	};

}

