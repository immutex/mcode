#include "mcode/core/registry.hxx"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace mcode {

	auto to_string( const tool_class klass ) noexcept -> std::string_view {
		switch ( klass ) {
			case tool_class::read: return "read";
			case tool_class::write: return "write";
			case tool_class::exec: return "exec";
			case tool_class::net: return "net";
			case tool_class::spawn: return "spawn";
			case tool_class::mcp: return "mcp";
		}

		return "read";
	}

	auto to_string( const tool_source source ) noexcept -> std::string_view {
		switch ( source ) {
			case tool_source::core: return "core";
			case tool_source::bundled_extension: return "bundled";
			case tool_source::project_extension: return "project";
			case tool_source::user_extension: return "user";
			case tool_source::mcp: return "mcp";
		}

		return "unknown";
	}

	auto tool_registry::add( tool_def definition ) -> status {
		if ( definition.name.empty( ) ) {
			return std::unexpected( fail( errc::config, "tool name must not be empty" ) );
		}

		if ( !definition.is_core( ) && definition.owner.empty( ) ) {
			return std::unexpected(
				fail( errc::config,
					"non-core tool '" + definition.name + "' must name its owner" ) );
		}

		const auto [ entry, inserted ] = tools_.try_emplace( definition.name,
			std::move( definition ) );

		if ( !inserted ) {
			return std::unexpected( fail( errc::config,
				"duplicate tool name '" + entry->first + "' from " +
					std::string{ to_string( entry->second.source ) } +
					( entry->second.owner.empty( ) ? "" : ":" + entry->second.owner ) ) );
		}

		return { };
	}

	auto tool_registry::find( const std::string_view name ) const -> const tool_def* {
		const auto entry = tools_.find( name );

		return entry == tools_.end( ) ? nullptr : &entry->second;
	}

	auto tool_registry::all( ) const -> std::vector< const tool_def* > {
		auto out = std::vector< const tool_def* >{ };
		out.reserve( tools_.size( ) );

		for ( const auto& [ name, definition ] : tools_ ) {
			out.push_back( &definition );
		}

		std::sort( out.begin( ), out.end( ),
			[]( const tool_def* left,
				const tool_def* right ) { return left->name < right->name; } );

		return out;
	}

	auto tool_registry::owned_by( const std::string_view owner ) const
		-> std::vector< const tool_def* > {
		auto out = std::vector< const tool_def* >{ };

		for ( const auto& [ name, definition ] : tools_ ) {
			if ( definition.owner == owner ) {
				out.push_back( &definition );
			}
		}

		std::sort( out.begin( ), out.end( ),
			[]( const tool_def* left,
				const tool_def* right ) { return left->name < right->name; } );

		return out;
	}

	auto tool_registry::remove( const std::string_view name ) -> bool {
		if ( name.empty( ) ) {
			return false;
		}

		return tools_.erase( std::string{ name } ) > 0;
	}

	auto tool_registry::remove_owner( const std::string_view owner ) -> std::size_t {
		if ( owner.empty( ) ) {
			return 0;
		}

		return std::erase_if( tools_,
			[owner]( const auto& entry ) { return entry.second.owner == owner; } );
	}

}
