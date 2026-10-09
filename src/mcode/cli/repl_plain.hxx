#pragma once

#include <string>
#include <vector>

// The non-TUI session path. `run_repl` falls back to it when the terminal is
// plain, absent, or cannot be taken over: one prompt per line on stdin.
[[nodiscard]] auto run_plain_repl( const std::vector< std::string >& arguments ) -> int;
