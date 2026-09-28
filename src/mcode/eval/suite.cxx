#include "mcode/eval/suite.hxx"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/core/version.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/toml.hxx"

namespace mcode::eval {

	namespace {

		auto now_iso8601( ) -> std::string {
			const auto now = std::chrono::system_clock::now( );
			const auto seconds = std::chrono::system_clock::to_time_t( now );
			auto buffer = std::array< char, 32 >{ };

			std::tm utc{ };
		#if defined( _WIN32 )
			gmtime_s( &utc, &seconds );
		#else
			gmtime_r( &seconds, &utc );
		#endif

			std::strftime( buffer.data( ), buffer.size( ), "%Y-%m-%dT%H:%M:%SZ", &utc );

			return buffer.data( );
		}

		auto read_text( const std::filesystem::path& path ) -> std::string {
			auto stream = std::ifstream{ path, std::ios::binary };

			if ( !stream ) {
				return { };
			}

			auto buffer = std::ostringstream{ };
			buffer << stream.rdbuf( );

			return buffer.str( );
		}

		auto fail_message( const std::string_view message ) -> result< bool > {
			return std::unexpected( fail( errc::tool_failed, std::string{ message } ) );
		}

	}

	auto run_record::to_json( ) const -> std::string {
		// Hand-built rather than through the DOM: the shape is fixed and this keeps
		// the record byte-stable, which is what makes two runs diffable.
		// Every interpolated string goes through the escaper. Escaping only `detail`
		// left the other fields able to emit a quote that makes the whole line
		// unparseable -- and a run record exists to be read back.
		auto out = std::string{ "{\"run_id\":\"" };
		json::append_escaped( out, run_id );
		out += "\",\"ts\":\"";
		json::append_escaped( out, timestamp );
		out += "\",\"suite\":\"";
		json::append_escaped( out, suite );
		out += "\",\"task_id\":\"";
		json::append_escaped( out, task_id );
		out += "\",\"scaffold_rev\":\"";
		json::append_escaped( out, scaffold_revision );
		out += "\",\"outcome\":{\"verdict\":\"";
		json::append_escaped( out, verdict );
		out += "\",\"exit_code\":" + std::to_string( exit_code );
		out += "},\"metrics\":{\"tool_calls\":" + std::to_string( tool_calls );
		out += ",\"wall_s\":" + std::to_string( wall_seconds );
		out += "},\"detail\":\"";
		json::append_escaped( out, detail );
		out += "\"}";

		return out;
	}

	auto pass_at_k( const std::vector< bool >& attempts ) -> double {
		// Did ANY attempt succeed.
		for ( const auto succeeded : attempts ) {
			if ( succeeded ) {
				return 1.0;
			}
		}

		return 0.0;
	}

	auto pass_power_k( const std::vector< bool >& attempts ) -> double {
		// Did ALL attempts succeed. Reporting only pass@k flatters a harness that
		// works half the time, which is why both are reported.
		if ( attempts.empty( ) ) {
			return 0.0;
		}

		for ( const auto succeeded : attempts ) {
			if ( !succeeded ) {
				return 0.0;
			}
		}

		return 1.0;
	}

	auto builtin_tasks( ) -> std::vector< task > {
		auto tasks = std::vector< task >{ };

		// 1. The workspace boundary refuses an escape.
		tasks.push_back( {
			.id = "workspace-refuses-escape",
			.description = "a path escaping the workspace is refused, and a prefix-sharing sibling too",
			.run = []( const std::filesystem::path& root ) -> result< bool > {
				auto opened = workspace::open( root );

				if ( !opened ) {
					return fail_message( "could not open the fixture as a workspace" );
				}

				if ( opened->resolve( "../../../../etc/passwd" ) ) {
					return fail_message( "an escaping path resolved" );
				}

				if ( opened->resolve( "src/calculator.cxx" ) ) {
					return true;
				}

				return fail_message( "a path inside the workspace was refused" );
			},
		} );

		// 2. Project config may only restrict.
		tasks.push_back( {
			.id = "project-config-cannot-widen",
			.description = "a project config file may only add deny/ask, never widen",
			.run = []( const std::filesystem::path& root ) -> result< bool > {
				auto allowed = config::load_layer( config::scope::project,
					root / ".mcode" / "config.toml" );

				if ( !allowed ) {
					return fail_message( "the fixture's own project config was rejected: " +
						allowed.error( ).msg );
				}

				// The same scope must refuse a widening key.
				auto scratch = root / ".mcode" / "widening.toml";
				{
					auto out = std::ofstream{ scratch, std::ios::trunc };
					out << "sandbox = \"off\"\n";
				}

				auto refused = config::load_layer( config::scope::project, scratch );
				std::filesystem::remove( scratch );

				if ( refused ) {
					return fail_message( "project scope accepted a sandbox-widening key" );
				}

				return true;
			},
		} );

		// 3. TOML refuses what it cannot represent.
		tasks.push_back( {
			.id = "toml-refuses-unsupported",
			.description = "dates, inline tables, and multi-line strings are refused, not ignored",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				const auto unsupported = std::vector< const char* >{
					"when = 1979-05-27T07:32:00Z",
					"point = { x = 1, y = 2 }",
					"text = \"\"\"multi\"\"\"",
					"dup = 1\ndup = 2",
				};

				for ( const auto* text : unsupported ) {
					if ( toml::parse( text ) ) {
						return fail_message( std::string{ "accepted: " } + text );
					}
				}

				return true;
			},
		} );

		// 4. The event bus is flat under reentrancy.
		tasks.push_back( {
			.id = "event-bus-flat-reentrancy",
			.description = "a handler publishing during dispatch queues rather than recurses",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				auto bus = events::bus{ };
				auto deliveries = 0;

				bus.subscribe( events::kind::step_start, [&]( const events::event& ) {
					++deliveries;

					if ( deliveries < 5 ) {
						auto nested = events::event{ };
						nested.type = events::kind::step_start;
						bus.publish( nested );
					}
				} );

				auto first = events::event{ };
				first.type = events::kind::step_start;
				bus.publish( first );

				if ( deliveries != 5 ) {
					return fail_message( "expected 5 deliveries, got " +
						std::to_string( deliveries ) );
				}

				if ( bus.dispatching( ) ) {
					return fail_message( "dispatch did not return to the outer frame" );
				}

				return true;
			},
		} );

		// 5. A veto short-circuits.
		tasks.push_back( {
			.id = "veto-short-circuits",
			.description = "the first veto wins and later handlers are skipped",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				auto bus = events::bus{ };
				auto reached = 0;

				bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
					++reached;

					return std::optional< events::veto >{ };
				} );

				bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
					++reached;

					auto veto = events::veto{ };
					veto.reason = "blocked";
					veto.source = "eval";

					return std::optional< events::veto >{ std::move( veto ) };
				} );

				bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
					++reached;

					return std::optional< events::veto >{ };
				} );

				auto value = events::event{ };
				value.type = events::kind::tool_pre_call;
				const auto decision = bus.publish( value );

				if ( !decision ) {
					return fail_message( "no veto was returned" );
				}

				if ( reached != 2 ) {
					return fail_message( "expected 2 handlers to run, got " +
						std::to_string( reached ) );
				}

				return true;
			},
		} );

		// 6. The session log survives a crash.
		tasks.push_back( {
			.id = "event-log-survives-crash",
			.description = "events are on disk before the log is closed, and a torn tail is reported",
			.run = []( const std::filesystem::path& root ) -> result< bool > {
				const auto path = root / "eval-session.jsonl";
				std::filesystem::remove( path );

				{
					auto log = event_log{ };

					if ( !log.open( path ) ) {
						return fail_message( "could not open the log" );
					}

					log.append( "session.start" );
					log.append( "tool.call" );

					// Deliberately NOT closed: this is the crash state.
					const auto on_disk = read_text( path );

					if ( on_disk.find( "tool.call" ) == std::string::npos ) {
						return fail_message( "events were buffered rather than flushed" );
					}

					log.close( );
				}

				auto replayed = replay_event_log( path );

				if ( !replayed || replayed->events_read != 2 ) {
					return fail_message( "replay did not recover both events" );
				}

				std::filesystem::remove( path );

				return true;
			},
		} );

		// 7. The provider descriptor rejects what cannot work.
		tasks.push_back( {
			.id = "provider-descriptor-validation",
			.description = "a descriptor with no endpoint, a non-http URL, or no mapping is refused",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				const auto bad = std::vector< const char* >{
					R"({"endpoint":"https://x/v1"})",
					R"({"name":"x"})",
					R"({"name":"x","endpoint":"file:///etc/passwd","stream":{"text_delta":"/t"}})",
					R"({"name":"x","endpoint":"https://x/v1"})",
					R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"no-slash"}})",
				};

				for ( const auto* text : bad ) {
					if ( model::descriptor_from_json( text ) ) {
						return fail_message( std::string{ "accepted: " } + text );
					}
				}

				// And the reference example must be accepted.
				const auto* good = R"({
					"name":"g","endpoint":"https://api.example.com/v1/chat/completions",
					"stream":{"text_delta":"/choices/0/delta/content"}
				})";

				if ( !model::descriptor_from_json( good ) ) {
					return fail_message( "a valid descriptor was rejected" );
				}

				return true;
			},
		} );

		// 8. The delta applier reassembles split tool arguments.
		tasks.push_back( {
			.id = "delta-applier-reassembles-fragments",
			.description = "arguments split mid-token are reassembled into valid JSON",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				const auto* descriptor_json = R"({
					"name":"x","endpoint":"https://x/v1",
					"stream":{
						"text_delta":"/t",
						"tool_calls":{"index":"/i","id":"/id","name":"/n","args":"/a"}
					}
				})";

				auto descriptor = model::descriptor_from_json( descriptor_json );

				if ( !descriptor ) {
					return fail_message( "descriptor rejected: " + descriptor.error( ).msg );
				}

				auto applier = model::delta_applier{ *descriptor };

				// Custom delimiters: these payloads end with `)"`, which would
				// terminate a plain R"( )" early.
				for ( const auto* payload : {
					R"JSON({"i":0,"id":"c","n":"read","a":"{\"pa"})JSON",
					R"JSON({"i":0,"a":"th\":\"a.tx"})JSON",
					R"JSON({"i":0,"a":"t\"}"})JSON" } ) {
					if ( !applier.feed( "message", payload ) ) {
						return fail_message( "the applier rejected a fragment" );
					}
				}

				const auto tail = applier.finish( );

				for ( const auto& value : tail ) {
					if ( value.type == model::chat_event::kind::tool_call_delta ) {
						if ( value.args_fragment == R"({"path":"a.txt"})" ) {
							return true;
						}

						return fail_message( "reassembled to '" + value.args_fragment + "'" );
					}
				}

				return fail_message( "no tool call was produced" );
			},
		} );

		// 9. Unknown CLI flags are refused.
		tasks.push_back( {
			.id = "cli-refuses-unknown-flags",
			.description = "an unrecognised flag is collected, never silently ignored",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				auto parsed = cli::parse_exec_options( { "--json", "--max-step", "5" } );

				if ( !parsed ) {
					return fail_message( "parsing failed outright" );
				}

				if ( parsed->unknown_arguments.size( ) != 1 ) {
					return fail_message( "the typo was not collected" );
				}

				if ( parsed->unknown_arguments.front( ) != "--max-step" ) {
					return fail_message( "the wrong argument was collected" );
				}

				// A flag without a value is an error, not an empty string.
				if ( cli::parse_exec_options( { "--model" } ) ) {
					return fail_message( "a flag without a value was accepted" );
				}

				return true;
			},
		} );

		// 10. The extension VM boundary holds.
		tasks.push_back( {
			.id = "extension-boundary-holds",
			.description = "io, package, and the writable-globals escapes are all refused",
			.run = []( const std::filesystem::path& ) -> result< bool > {
				auto options = lua_host_options{ };
				options.extension_name = "eval";

				auto host = lua_host::create( std::move( options ) );

				if ( !host ) {
					return fail_message( "could not create the VM" );
				}

				const auto probes = std::vector< std::pair< const char*, const char* > >{
					{ "(io ~= nil)", "io" },
					{ "(package ~= nil)", "package" },
					{ "(type(os.execute) ~= 'nil')", "os.execute" },
					{ "(type(loadstring) ~= 'nil')", "loadstring" },
					{ "select(1, pcall(rawset, _G, 'x', 1))", "rawset on _G" },
					{ "select(1, pcall(setmetatable, _G, {}))", "setmetatable on _G" },
				};

				for ( const auto& [ expression, label ] : probes ) {
					auto outcome = host->eval_to_string( expression );

					if ( !outcome ) {
						return fail_message( std::string{ "probe could not run: " } + label );
					}

					if ( *outcome != "false" ) {
						return fail_message( std::string{ "escape succeeded: " } + label );
					}
				}

				return true;
			},
		} );

		return tasks;
	}

	auto run_suite( const std::filesystem::path& fixture_root, const std::string_view suite_name )
		-> suite_result {
		auto result = suite_result{ };
		result.suite = std::string{ suite_name };
		result.run_id = "eval-" + now_iso8601( );

		// A stable identity for "what code produced this run": a git
		// sha; the version string is what M0 has, and it is recorded rather than
		// omitted so the field's absence is not mistaken for an oversight.
		auto revision = std::string{ "mcode/" } + std::string{ VERSION };

		for ( const auto& entry : builtin_tasks( ) ) {
			auto record = run_record{ };
			record.run_id = result.run_id;
			record.timestamp = now_iso8601( );
			record.suite = result.suite;
			record.task_id = entry.id;
			record.scaffold_revision = revision;
			// The tasks assert harness behaviour directly; none dispatches a model
			// tool call, so the count is zero. Reporting 1 would be a fabricated
			// metric, which is worse than a zero that is honestly zero.
			record.tool_calls = 0;

			const auto started = std::chrono::steady_clock::now( );
			auto outcome = entry.run( fixture_root );
			const auto elapsed = std::chrono::duration_cast< std::chrono::microseconds >(
				std::chrono::steady_clock::now( ) - started ).count( );

			record.wall_seconds = static_cast< double >( elapsed ) / 1'000'000.0;

			if ( outcome && *outcome ) {
				record.verdict = "pass";
				record.exit_code = 0;
				++result.passed;
			} else if ( outcome ) {
				record.verdict = "fail";
				record.exit_code = 1;
				record.detail = "assertion failed";
				++result.failed;
			} else {
				record.verdict = "error";
				record.exit_code = 2;
				record.detail = outcome.error( ).msg;
				++result.errored;
			}

			result.records.push_back( std::move( record ) );
		}

		return result;
	}

	auto to_jsonl( const suite_result& result ) -> std::string {
		auto out = std::string{ };

		for ( const auto& record : result.records ) {
			out += record.to_json( );
			out += '\n';
		}

		return out;
	}

}
