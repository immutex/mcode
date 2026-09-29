#include "mcode/perm/permission.hxx"

#include "mcode/perm/argv.hxx"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/logging.hxx"

namespace mcode::perm {

	namespace {

		// The floor's exec rules, as first tokens. `sudo`, `doas`, `runas` as
		// the first token is escalation; the same word as an argument is not.
		inline constexpr auto FLOOR_PROGRAMS = std::array< std::string_view, 7 >{
			"sudo", "sudo.exe", "doas", "doas.exe", "runas", "runas.exe", "mkfs",
		};

		// mkfs family: mkfs.ext4, mkfs.ntfs, ... all share the `mkfs.` prefix
		// and there is no legitimate reason for an agent to invoke one. The
		// near-miss (`man mkfs.ext4`) has a different first token.
		inline constexpr std::string_view MKFS_PREFIX = "mkfs.";

		// Partition/format tools. `format` on Windows; the same word as an
		// argument (`man format`) has a different first token.
		inline constexpr auto FLOOR_FORMAT_PROGRAMS = std::array< std::string_view, 6 >{
			"fdisk", "fdisk.exe", "diskpart", "diskpart.exe", "format", "format.exe",
		};

		// Recursive-delete tokens that make the target a deletion target.
		inline constexpr std::string_view RM_PROGRAM = "rm";
		inline constexpr std::string_view RM_RECURSE_FORCE = "-rf";
		inline constexpr std::string_view RM_FORCE_RECURSE = "-fr";

		// `dd`'s output-file option.
		inline constexpr std::string_view DD_PROGRAM = "dd";
		inline constexpr std::string_view DD_OF_PREFIX = "of=";

		// Device-path prefixes for the dd target check.
		inline constexpr std::string_view DEV_PREFIX = "/dev/";
		inline constexpr std::string_view NVME_PREFIX = "/dev/nvme";
		inline constexpr std::string_view PHYSICALDRIVE_PREFIX = "\\\\.\\physicaldrive";

		// `~` and the env-var spellings of home. `$HOME` cannot appear in parsed
		// argv (the parser refuses `$`), so the token arrives only as a literal.
		inline constexpr std::string_view TILDE = "~";
		inline constexpr std::string_view TILDE_SLASH = "~/";
		inline constexpr std::string_view HOME_ENV = "HOME";

		[[nodiscard]] auto lower_ascii( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			for ( auto& character : out ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return out;
		}

		[[nodiscard]] auto token_equals( const std::string_view token,
			const std::string_view expected ) -> bool {
			return lower_ascii( token ) == lower_ascii( expected );
		}

		// The first token, path-stripped and lowercased -- the same
		// normalisation is_exec_runner applies, so `/bin/sudo` and `SUDO` land
		// on the floor.
		[[nodiscard]] auto program_name( const std::vector< std::string >& argv )
			-> std::string {
			const auto& raw = argv.front( );
			const auto slash = raw.find_last_of( "/\\" );
			const auto base = slash == std::string::npos
				? raw
				: std::string_view{ raw }.substr( slash + 1 );

			return lower_ascii( base );
		}

		// True when the token names a filesystem root. Unconditional: it must not
		// depend on the home directory being resolvable, or `rm -rf /` stops
		// firing the floor on a machine with no HOME set -- and then `--yolo`
		// turns the resulting `ask` into an allow.
		[[nodiscard]] auto is_filesystem_root( const std::filesystem::path& target ) -> bool {
			return target == target.root_path( );
		}

		// True when the token names the user's home directory. The canonical form
		// is what makes `rm -rf /`, `rm -rf //` and `rm -rf /./` one command; the
		// tilde spelling resolves to the same directory and is caught by the same
		// comparison.
		[[nodiscard]] auto is_home_directory( const std::filesystem::path& target,
			const std::filesystem::path& home ) -> bool {
			return !home.empty( ) && ( target == home || target == home.root_path( ) );
		}

		// Resolves an argv token to a canonical path. Relative tokens resolve
		// against the workspace root, which is where exec runs.
		[[nodiscard]] auto resolve_target( const std::string& token,
			const mcode::workspace& space ) -> std::optional< std::filesystem::path > {
			auto candidate = std::filesystem::path{ token };

			if ( candidate.is_relative( ) ) {
				candidate = space.root( ) / candidate;
			}

			auto canonical = platform::canonicalize( candidate );

			if ( !canonical ) {
				return std::nullopt;
			}

			return *canonical;
		}

		// The home directory, canonicalized. Empty when HOME is unset -- the
		// floor's home rules then simply do not fire, which is fail-closed for
		// a rule whose subject does not exist on this machine.
		[[nodiscard]] auto home_directory( ) -> std::filesystem::path {
			const auto* value = std::getenv( "HOME" );
			const auto* alt = std::getenv( "USERPROFILE" );

			const auto raw = std::filesystem::path{
				value != nullptr && *value != '\0' ? value
				: alt != nullptr ? alt
				: "" };

			if ( raw.empty( ) ) {
				return { };
			}

			auto canonical = platform::canonicalize( raw );

			return canonical ? *canonical : std::filesystem::path{ };
		}

		// --- floor checks, one per rule ------------------------------------

		// Recursive delete of a filesystem root or the user's home.
		[[nodiscard]] auto floor_recursive_delete( const std::vector< std::string >& argv,
			const mcode::workspace& space ) -> std::optional< std::string > {
			if ( !token_equals( argv.front( ), RM_PROGRAM ) ) {
				return std::nullopt;
			}

			auto recursive = false;
			auto targets = std::vector< std::string >{ };

			for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
				const auto& token = argv[ index ];

				if ( token_equals( token, RM_RECURSE_FORCE ) ||
					token_equals( token, RM_FORCE_RECURSE ) ) {
					recursive = true;

					continue;
				}

				if ( token.starts_with( '-' ) ) {
					continue;
				}

				targets.push_back( token );
			}

			if ( !recursive || targets.empty( ) ) {
				return std::nullopt;
			}

			const auto home = home_directory( );

			for ( const auto& target : targets ) {
				// A literal `~` or `~/...` resolves against the real home, not
				// the workspace: `rm -rf ~` must fire even though the shell
				// would expand it to a path outside the workspace.
				if ( target == TILDE || target.starts_with( TILDE_SLASH ) ) {
					if ( !home.empty( ) ) {
						const auto expanded = target == TILDE
							? home
							: home / std::filesystem::path{ target.substr( TILDE_SLASH.size( ) ) };

						// The floor names the home itself, not its contents:
						// `rm -rf ~/scratch/project-a` is the near-miss that
						// must stay allowed. Compare the canonical expansion
						// against the home, component-wise.
						auto canonical = platform::canonicalize( expanded );

						if ( canonical && *canonical == home ) {
							return "recursive delete of the home directory";
						}
					}

					continue;
				}

				if ( token_equals( target, HOME_ENV ) ) {
					// A literal `HOME` token is a relative path named HOME, not
					// the environment variable -- the parser already refused
					// `$HOME`. Do not treat it as the home directory.
					continue;
				}

				const auto resolved = resolve_target( target, space );

				if ( !resolved ) {
					continue;
				}

				if ( is_filesystem_root( *resolved ) ) {
					return "recursive delete of a filesystem root";
				}

				if ( is_home_directory( *resolved, home ) ) {
					return "recursive delete of the home directory";
				}
			}

			return std::nullopt;
		}

		// Privilege escalation: sudo / doas / runas as the first token.
		[[nodiscard]] auto floor_privilege_escalation(
			const std::vector< std::string >& argv ) -> std::optional< std::string > {
			const auto program = program_name( argv );

			for ( const auto candidate : FLOOR_PROGRAMS ) {
				if ( program == candidate ) {
					return "privilege escalation";
				}
			}

			return std::nullopt;
		}

		// Filesystem creation: mkfs and the mkfs.* family as the first token.
		[[nodiscard]] auto floor_filesystem_creation(
			const std::vector< std::string >& argv ) -> std::optional< std::string > {
			const auto program = program_name( argv );

			if ( program == "mkfs" || program.starts_with( MKFS_PREFIX ) ) {
				return "filesystem creation";
			}

			return std::nullopt;
		}

		// Partition/format tools as the first token.
		[[nodiscard]] auto floor_partition_tools( const std::vector< std::string >& argv )
			-> std::optional< std::string > {
			const auto program = program_name( argv );

			for ( const auto candidate : FLOOR_FORMAT_PROGRAMS ) {
				if ( program == candidate ) {
					return "partition or format tool";
				}
			}

			return std::nullopt;
		}

		// Raw write to a device: dd whose of= target is a device path.
		[[nodiscard]] auto floor_raw_device_write( const std::vector< std::string >& argv )
			-> std::optional< std::string > {
			if ( !token_equals( argv.front( ), DD_PROGRAM ) ) {
				return std::nullopt;
			}

			for ( auto index = std::size_t{ 1 }; index < argv.size( ); ++index ) {
				const auto& token = argv[ index ];

				if ( !token.starts_with( DD_OF_PREFIX ) ) {
					continue;
				}

				const auto target = lower_ascii( token.substr( DD_OF_PREFIX.size( ) ) );

				if ( target.starts_with( DEV_PREFIX ) || target.starts_with( NVME_PREFIX ) ||
					target.starts_with( PHYSICALDRIVE_PREFIX ) ) {
					return "raw write to a device";
				}
			}

			return std::nullopt;
		}

		// Which store section a request belongs to, and the key within it. One
		// function for both the write and the read: when the two disagreed, a
		// remembered answer was written to a section nothing ever looked in.
		//
		// Exec keys on the canonical argv, never a prefix -- a remembered
		// `git push` must not authorize `git push --force`. Everything else
		// keys on the tool name, because its resource is an opaque argument
		// blob rather than a stable identity.
		enum class store_section { exec, paths, tools };

		struct store_key {
			store_section section = store_section::tools;
			std::string key;
		};

		[[nodiscard]] auto store_key_for( const permission_request& request )
			-> std::optional< store_key > {
			if ( request.resource.empty( ) && request.klass == tool_class::exec ) {
				// An unparsable command has no canonical form to remember.
				return std::nullopt;
			}

			if ( request.klass == tool_class::exec ) {
				return store_key{ store_section::exec, request.resource };
			}

			if ( request.klass == tool_class::read || request.klass == tool_class::write ) {
				// The user's own `paths` entries are hand-written globs; we
				// never auto-persist one, so there is nothing to key here.
				return std::nullopt;
			}

			if ( request.tool_name.empty( ) ) {
				return std::nullopt;
			}

			return store_key{ store_section::tools, request.tool_name };
		}

		using store_map = std::map< std::string, store_decision, std::less<> >;

		[[nodiscard]] auto section_of( store_layer& layer, const store_section section )
			-> store_map& {
			switch ( section ) {
				case store_section::exec: return layer.exec;
				case store_section::paths: return layer.paths;
				case store_section::tools: return layer.tools;
			}

			return layer.tools;
		}

		[[nodiscard]] auto section_of( const store_layer& layer, const store_section section )
			-> const store_map& {
			switch ( section ) {
				case store_section::exec: return layer.exec;
				case store_section::paths: return layer.paths;
				case store_section::tools: return layer.tools;
			}

			return layer.tools;
		}

	}

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
		if ( request.klass == tool_class::write ) {
			// Writing .git/ or .mcode/ internals. Matched on the canonical
			// path's first component below a boundary root, so `.gitignore` --
			// a different name -- is not caught.
			auto candidate = std::filesystem::path{ request.resource };

			if ( candidate.is_relative( ) ) {
				candidate = space_->root( ) / candidate;
			}

			const auto canonical = platform::canonicalize( candidate );

			// A path that cannot be canonicalized cannot be shown to be outside
			// the protected set, so it is refused. Falling through here would
			// let an unresolvable path skip the floor entirely.
			if ( !canonical ) {
				return "write to an unresolvable path";
			}

			if ( !space_->is_protected( *canonical ) ) {
				return std::nullopt;
			}

			return "write to protected harness internals";
		}

		if ( request.klass != tool_class::exec ) {
			return std::nullopt;
		}

		// The caller hands us the canonical argv as one string. Re-parsing with
		// the same parser the tool layer used is what keeps a quoted token
		// containing a space intact -- splitting on ' ' would turn one token
		// into two and resolve a path that was never named.
		const auto parsed = parse_command_line( request.resource );

		if ( !parsed || parsed->empty( ) ) {
			return std::nullopt;
		}

		const auto& argv = *parsed;

		if ( const auto hit = floor_privilege_escalation( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_filesystem_creation( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_partition_tools( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_raw_device_write( argv ) ) {
			return hit;
		}

		if ( const auto hit = floor_recursive_delete( argv, *space_ ) ) {
			return hit;
		}

		return std::nullopt;
	}

	auto permission_engine::rule_matches( const rule& candidate,
		const permission_request& request ) const -> bool {
		if ( candidate.klass != request.klass ) {
			return false;
		}

		// Exec rules match the canonical argv exactly, never a prefix: a
		// remembered `git status` must not authorize `git push`.
		if ( request.klass == tool_class::exec ) {
			return candidate.pattern == request.resource;
		}

		// Path rules are globs over the canonical path. `*` within a segment,
		// `**` across segments; a trailing `/*` does not cross into deeper
		// directories unless the pattern says `**`.
		if ( request.klass == tool_class::read || request.klass == tool_class::write ) {
			const auto path = std::string_view{ request.resource };

			auto pattern_parts = std::vector< std::string_view >{ };
			auto path_parts = std::vector< std::string_view >{ };

			const auto split = []( const std::string_view text ) {
				auto parts = std::vector< std::string_view >{ };
				auto start = std::size_t{ 0 };

				while ( start <= text.size( ) ) {
					const auto slash = text.find( '/', start );
					const auto end = ( slash == std::string_view::npos ) ? text.size( ) : slash;

					if ( end > start ) {
						parts.push_back( text.substr( start, end - start ) );
					}

					if ( slash == std::string_view::npos ) {
						break;
					}

					start = slash + 1;
				}

				return parts;
			};

			pattern_parts = split( candidate.pattern );
			path_parts = split( path );

			auto states = std::vector< std::pair< std::size_t, std::size_t > >{ { 0, 0 } };

			while ( !states.empty( ) ) {
				const auto [ pattern_index, path_index ] = states.back( );
				states.pop_back( );

				if ( pattern_index == pattern_parts.size( ) &&
					path_index == path_parts.size( ) ) {
					return true;
				}

				if ( pattern_index >= pattern_parts.size( ) ) {
					continue;
				}

				const auto& part = pattern_parts[ pattern_index ];

				if ( part == "**" ) {
					for ( auto skip = path_index; skip <= path_parts.size( ); ++skip ) {
						states.emplace_back( pattern_index + 1, skip );
					}

					continue;
				}

				if ( path_index >= path_parts.size( ) ) {
					continue;
				}

				// Segment match with `*` wildcard support.
				const auto& segment = path_parts[ path_index ];
				auto matched = false;

				if ( part.find( '*' ) == std::string_view::npos ) {
					matched = part == segment;
				} else {
					const auto wildcard_match = []( const std::string_view pattern,
						const std::string_view text ) {
						auto p = std::size_t{ 0 };
						auto t = std::size_t{ 0 };
						auto star = std::string_view::npos;
						auto after_star = std::size_t{ 0 };

						while ( t < text.size( ) ) {
							if ( p < pattern.size( ) &&
								( pattern[ p ] == text[ t ] || pattern[ p ] == '?' ) ) {
								++p;
								++t;

								continue;
							}

							if ( p < pattern.size( ) && pattern[ p ] == '*' ) {
								star = p++;
								after_star = t;

								continue;
							}

							if ( star != std::string_view::npos ) {
								p = star + 1;
								t = ++after_star;

								continue;
							}

							return false;
						}

						while ( p < pattern.size( ) && pattern[ p ] == '*' ) {
							++p;
						}

						return p == pattern.size( );
					};

					matched = wildcard_match( part, segment );
				}

				if ( matched ) {
					states.emplace_back( pattern_index + 1, path_index + 1 );
				}
			}

			return false;
		}

		// Tool-class rules (mcp, net, spawn) match the tool name exactly.
		return candidate.pattern == request.tool_name;
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
				const auto key = store_key_for( request );

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
		// workspace.
		switch ( request.klass ) {
			case tool_class::read: {
				if ( options_.approval == "always" ) {
					return { permission_decision::ask,
						{ "default", "approval = always", permission_decision::ask },
						"approval = always prompts for every tool" };
				}

				auto candidate = std::filesystem::path{ request.resource };

				if ( candidate.is_relative( ) ) {
					candidate = space_->root( ) / candidate;
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
		// yolo disables prompts, not policy: every ask resolves to allow
		// whatever it is, but the floor and permissions.deny already decided
		// before this point.
		if ( options_.yolo ) {
			last_verdict_ = { permission_decision::allow,
				{ "yolo", matched.pattern, permission_decision::allow },
				"yolo resolves the prompt to allow" };

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
				// outside the workspace is never auto-persisted (`12`), and
				// `store_key_for` returns nothing for it.
				const auto key = store_key_for( request );

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
		if ( store_ == nullptr ) {
			return { };
		}

		const auto loaded = store_->load( );

		if ( !loaded ) {
			return std::unexpected( loaded.error( ) );
		}

		if ( !loaded->has_value( ) ) {
			return { };
		}

		stored_ = **loaded;

		// A project-scoped store cannot widen: drop every allow it carries,
		// with a warning, because a cloned repo must not be able to grant
		// itself permissions. Denies survive. The store's origin decides: a
		// file under the workspace root is the project's.
		const auto store_text = store_->file( ).generic_string( );
		const auto root_text = space_->root( ).generic_string( );

		if ( store_text.starts_with( root_text + "/" ) ||
			store_text.starts_with( root_text + "\\" ) ) {
			auto dropped = std::size_t{ 0 };

			const auto filter = [ &dropped ]( auto& entries ) {
				for ( auto entry = entries.begin( ); entry != entries.end( ); ) {
					if ( entry->second == store_decision::allow ) {
						entry = entries.erase( entry );
						++dropped;

						continue;
					}

					++entry;
				}
			};

			filter( stored_.exec );
			filter( stored_.paths );
			filter( stored_.tools );

			if ( dropped > 0 ) {
				if ( logging_initialized( ) ) {
					logger( )->warn( "project store {} cannot widen; dropped {} allow entries",
						store_text, dropped );
				}

				warnings_.push_back( "project store " + store_text +
					" cannot widen; dropped " + std::to_string( dropped ) +
					" allow entries" );
			}
		}

		return { };
	}

}

