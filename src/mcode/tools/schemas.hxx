#pragma once

#include <string_view>

namespace mcode::tools {

	// flat parameter schemas only: validate_schema accepts top-level properties, no nesting
	inline constexpr std::string_view READ_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File to read, relative to the workspace root. Example: src/main.cxx. Directories and URLs are refused."
    },
    "offset": {
      "type": "integer",
      "description": "1-based first line to return. Default 1. Example: 200 to resume a truncated read."
    },
    "limit": {
      "type": "integer",
      "description": "Number of lines to return. Default 100, maximum 1000. Example: 50."
    }
  },
  "required": [ "path" ]
})JSON";

	inline constexpr std::string_view WRITE_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File to create or overwrite, relative to the workspace root. Example: src/notes.md. Overwriting an existing file requires reading it first in this session; creating a new file does not. Maximum 10 MiB."
    },
    "content": {
      "type": "string",
      "description": "The complete new file content, replacing any prior content. Example: the full text of the file including a trailing newline."
    }
  },
  "required": [ "path", "content" ]
})JSON";

	inline constexpr std::string_view EDIT_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File to edit, relative to the workspace root. Must have been read this session. Example: src/main.cxx."
    },
    "old_string": {
      "type": "string",
      "description": "Exact text to replace. Must appear exactly once unless replace_all is set; empty is refused. Example: a unique function name plus its signature line."
    },
    "new_string": {
      "type": "string",
      "description": "Replacement text, same length or different. Example: the edited version of the anchored lines."
    },
    "replace_all": {
      "type": "boolean",
      "description": "Replace every occurrence of old_string instead of requiring uniqueness. Default false."
    }
  },
  "required": [ "path", "old_string", "new_string" ]
})JSON";

	inline constexpr std::string_view GLOB_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "pattern": {
      "type": "string",
      "description": "Glob pattern relative to the workspace root; ** crosses directories. Example: src/**/*.cxx. .git and .mcode are always skipped."
    },
    "max_results": {
      "type": "integer",
      "description": "Maximum paths returned. Default 1000. Example: 50 for a quick look."
    }
  },
  "required": [ "pattern" ]
})JSON";

	inline constexpr std::string_view GREP_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "pattern": {
      "type": "string",
      "description": "ECMAScript regex to search file contents for. Example: void handle_.*\\( ."
    },
    "path": {
      "type": "string",
      "description": "Directory or file to search, relative to the workspace root. Default the whole workspace. Example: src."
    },
    "glob": {
      "type": "string",
      "description": "Glob filter on file names, such as *.cxx. Default all non-binary files."
    },
    "max_matches": {
      "type": "integer",
      "description": "Maximum matches returned. Default 50. Example: 20."
    }
  },
  "required": [ "pattern" ]
})JSON";

	inline constexpr std::string_view BASH_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "command": {
      "type": "string",
      "description": "Single shell command to run in the workspace root. Compound commands (&& | ; $() backticks) are refused by the approval gate. Example: cmake --build build/Release."
    },
    "timeout_ms": {
      "type": "integer",
      "description": "Milliseconds before the process is killed. Default 60000, maximum 600000. Example: 120000 for a slow build."
    }
  },
  "required": [ "command" ]
})JSON";

	inline constexpr std::string_view ASK_USER_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "question": {
      "type": "string",
      "description": "The question to put to the user, self-contained. Example: Should the retry loop live in fetch_page or the caller?"
    },
    "options": {
      "type": "array",
      "items": { "type": "string" },
      "description": "Suggested answers to choose from. Example: [ \"fetch_page\", \"the caller\" ]."
    }
  },
  "required": [ "question" ]
})JSON";

	inline constexpr std::string_view TOOL_SEARCH_SCHEMA = R"JSON({
  "type": "object",
  "properties": {
    "query": {
      "type": "string",
      "description": "Words to match against tool names and descriptions. Example: git commit."
    },
    "expand": {
      "type": "boolean",
      "description": "Return the full JSON schema of each match instead of name and one-line description. Default false."
    }
  },
  "required": [ "query" ]
})JSON";

}
