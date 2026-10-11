#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode {

	// The file is the record; this only bounds what the in-memory copy holds.
	inline constexpr std::uint64_t MAX_RETAINED_LOG_BYTES = 16u * 1024u * 1024u;

	struct event {
		std::uint32_t version = 1;
		std::uint64_t sequence = 0;
		std::int64_t timestamp_ms = 0;
		std::string kind;
		std::string run;
		std::uint32_t turn = 0;
		std::uint32_t step = 0;
		std::string payload_json;

		[[nodiscard]] auto to_json( ) const -> std::string;
	};

	// Flushed per event so a crash keeps every complete line; replay tolerates a torn tail.
	class event_log {
		public:
		event_log( ) = default;
		~event_log( );

		event_log( event_log&& other ) noexcept;
		auto operator=( event_log&& other ) noexcept -> event_log&;

		event_log( const event_log& ) = delete;
		auto operator=( const event_log& ) -> event_log& = delete;

		auto open( const std::filesystem::path& path ) -> status;
		auto close( ) -> void;

		[[nodiscard]] auto is_open( ) const noexcept -> bool { return sink_ != nullptr; }
		[[nodiscard]] auto path( ) const noexcept -> const std::filesystem::path& { return path_; }

		// Counts flush failures, malformed payloads and a torn tail; never thrown,
		// since losing the log must not end the session.
		[[nodiscard]] auto write_failures( ) const noexcept -> std::uint64_t {
			return write_failures_;
		}

		[[nodiscard]] auto evicted( ) const noexcept -> std::uint64_t { return evicted_; }

		auto set_position( std::string_view run, std::uint32_t turn, std::uint32_t step ) -> void {
			run_ = std::string{ run };
			turn_ = turn;
			step_ = step;
		}

		auto append( const std::string kind, std::string payload_json = "{}" ) -> event;

		// Not append(): this preserves the recorded sequence and must not renumber.
		auto restore( event recorded ) -> void;

		[[nodiscard]] auto events( ) const noexcept -> const std::vector< event >& {
			return events_;
		}
		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return events_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return events_.empty( ); }


		[[nodiscard]] auto next_sequence( ) const noexcept -> std::uint64_t {
			return next_sequence_;
		}


		[[nodiscard]] auto to_jsonl( ) const -> std::string;

		private:
		std::vector< event > events_;
		std::uint64_t next_sequence_ = 0;

		std::unique_ptr< std::FILE, void ( * )( std::FILE* ) > sink_{ nullptr, nullptr };
		std::filesystem::path path_;
		std::uint64_t write_failures_ = 0;

		std::uint64_t retained_bytes_ = 0;
		std::uint64_t evicted_ = 0;

		std::string run_;
		std::uint32_t turn_ = 0;
		std::uint32_t step_ = 0;
	};

	struct replay_result {
		event_log log;

		std::size_t events_read = 0;

		bool truncated_tail = false;

		std::size_t malformed_lines = 0;
	};

		[[nodiscard]] auto replay_event_log( const std::filesystem::path& path )
			-> result< replay_result >;

}
