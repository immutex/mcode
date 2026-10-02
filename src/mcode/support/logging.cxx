#include "mcode/support/logging.hxx"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <vector>

namespace mcode {

	namespace {

		constexpr std::size_t QUEUE_SIZE = 8192;
		constexpr std::size_t QUEUE_THREADS = 1;
		constexpr std::size_t MAX_FILE_BYTES = 8u * 1024u * 1024u;
		constexpr std::size_t MAX_ROTATED_FILES = 3;

		std::mutex g_log_mutex;
		std::shared_ptr< spdlog::logger > g_logger;
		bool g_initialized = false;

	}

	auto parse_log_level( const std::string_view name ) -> std::optional< log_level > {
		if ( name == "trace" ) return log_level::trace;
		if ( name == "debug" ) return log_level::debug;
		if ( name == "info" ) return log_level::info;
		if ( name == "warn" || name == "warning" ) return log_level::warn;
		if ( name == "error" ) return log_level::error;
		if ( name == "critical" ) return log_level::critical;
		if ( name == "off" ) return log_level::off;

		// absent, not "info": defaulting a typo would tell the user their setting took effect.
		return std::nullopt;
	}

	auto to_string( const log_level level ) noexcept -> std::string_view {
		switch ( level ) {
			case log_level::trace: return "trace";
			case log_level::debug: return "debug";
			case log_level::info: return "info";
			case log_level::warn: return "warn";
			case log_level::error: return "error";
			case log_level::critical: return "critical";
			case log_level::off: return "off";
		}

		return "info";
	}

	namespace {

		auto to_spdlog( const log_level level ) noexcept -> spdlog::level::level_enum {
			switch ( level ) {
				case log_level::trace: return spdlog::level::trace;
				case log_level::debug: return spdlog::level::debug;
				case log_level::info: return spdlog::level::info;
				case log_level::warn: return spdlog::level::warn;
				case log_level::error: return spdlog::level::err;
				case log_level::critical: return spdlog::level::critical;
				case log_level::off: return spdlog::level::off;
			}

			return spdlog::level::info;
		}

	}

	auto init_logging( const log_level level, const std::filesystem::path& log_directory ) -> status {
		const std::scoped_lock lock{ g_log_mutex };

		if ( g_initialized ) {
			g_logger->set_level( to_spdlog( level ) );

			return { };
		}

		auto sinks = std::vector< spdlog::sink_ptr >{ };
		sinks.push_back( std::make_shared< spdlog::sinks::stdout_color_sink_mt >( ) );

		// deferred: the stdout sink is still installed, so the message is visible.
		auto directory_error = std::string{ };

		if ( !log_directory.empty( ) ) {
			auto error_code = std::error_code{ };
			std::filesystem::create_directories( log_directory, error_code );

			if ( error_code ) {
				directory_error = "cannot create " + log_directory.string( ) + ": " +
					error_code.message( ) + "; logging to stdout only";
			} else {
				const auto path = log_directory / "mcode.log";
				sinks.push_back( std::make_shared< spdlog::sinks::rotating_file_sink_mt >(
					path.string( ), MAX_FILE_BYTES, MAX_ROTATED_FILES ) );
			}
		}

		spdlog::init_thread_pool( QUEUE_SIZE, QUEUE_THREADS );

		auto created = std::make_shared< spdlog::async_logger >( "mcode", sinks.begin( ), sinks.end( ),
			spdlog::thread_pool( ), spdlog::async_overflow_policy::overrun_oldest );

		created->set_level( to_spdlog( level ) );

		// `%z`, not a literal `Z`: spdlog renders local time, so a hardcoded Z mislabels lines.
		created->set_pattern( "%Y-%m-%dT%H:%M:%S.%e%z [%^%l%$] %v" );
		created->flush_on( spdlog::level::warn );

		spdlog::register_logger( created );
		spdlog::set_default_logger( created );

		g_logger = std::move( created );
		g_initialized = true;

		if ( !directory_error.empty( ) ) {
			return std::unexpected( fail( errc::io, directory_error ) );
		}

		return { };
	}

	auto shutdown_logging( ) -> void {
		const std::scoped_lock lock{ g_log_mutex };

		if ( !g_initialized ) {
			return;
		}

		if ( g_logger ) {
			g_logger->flush( );
		}

		g_logger.reset( );
		g_initialized = false;
		spdlog::shutdown( );
	}

	auto logging_initialized( ) noexcept -> bool {
		const std::scoped_lock lock{ g_log_mutex };

		return g_initialized;
	}

	auto logger( ) noexcept -> std::shared_ptr< spdlog::logger > {
		const std::scoped_lock lock{ g_log_mutex };

		return g_logger;
	}

}
