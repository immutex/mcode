#pragma once

// a denial returns `nil, err` rather than raising: the extension can handle it.

#include <string>
#include <string_view>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// the one place the denial message is spelled, so every gated entry reads identically.
	auto deny_permission( lua_State* state, const char* permission, const char* entry )
		-> int;

	[[nodiscard]] auto manifest_allows( const api_surface& surface,
		const char* permission ) -> bool;

	// the host an URL is addressed to, lowercased; empty when the URL does not parse.
	[[nodiscard]] auto url_host( std::string_view url ) -> std::string;

	// a declared `net:<host>` entry, exact: a wildcard would widen the declaration.
	[[nodiscard]] auto host_declared( const api_surface& surface, std::string_view host )
		-> bool;

	// a declared `credential:<NAME>` entry, exact.
	[[nodiscard]] auto credential_declared( const api_surface& surface,
		std::string_view name ) -> bool;

}
