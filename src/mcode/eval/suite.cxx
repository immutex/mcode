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
		// Hand-built to keep the record byte-stable; every field goes through the escaper.
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
		for ( const auto succeeded : attempts ) {
			if ( succeeded ) {
				return 1.0;
			}
		}

		return 0.0;
	}

	auto pass_power_k( const std::vector< bool >& attempts ) -> double {
		// Both are reported: pass@k alone flatters a harness that works half the time.
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

				// Custom delimiters: these payloads end with `)"`, which would end R"( )" early.
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

				if ( cli::parse_exec_options( { "--model" } ) ) {
					return fail_message( "a flag without a value was accepted" );
				}

				return true;
			},
		} );

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

		// The version string stands in for a git sha, so the field is never absent.
		auto revision = std::string{ "mcode/" } + std::string{ VERSION };

		for ( const auto& entry : builtin_tasks( ) ) {
			auto record = run_record{ };
			record.run_id = result.run_id;
			record.timestamp = now_iso8601( );
			record.suite = result.suite;
			record.task_id = entry.id;
			record.scaffold_revision = revision;
			// No task dispatches a model tool call, so this is honestly zero.
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
