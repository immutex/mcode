#include "mcode/cli/tier_config.hxx"

#include "mcode/model/capabilities.hxx"

namespace mcode::cli {

	namespace {

		// Empty when unset, which `tier_for` reads as "use the loop's own model".
		[[nodiscard]] auto resolve_one( const std::string_view default_model,
			const mcode::config::merged_config& config, const std::string_view key )
			-> mcode::result< mcode::tier_model > {
			const auto configured = config.get_string( key );

			if ( !configured || configured->empty( ) || *configured == default_model ) {
				return mcode::tier_model{ };
			}

			const auto caps = mcode::model::resolve_capabilities( *configured, &config );

			if ( !caps ) {
				return std::unexpected( mcode::fail( mcode::errc::config,
					"model '" + *configured + "' named by " + std::string{ key }
						+ " has no capabilities; price it under "
						+ mcode::model::capability_config_section( *configured )
						+ ", or unset the tier to use '" + std::string{ default_model }
						+ "'" ) );
			}

			return mcode::tier_model{ .name = *configured, .caps = *caps, .configured = true };
		}

	}

	auto resolve_tiers( const std::string_view default_model,
		const mcode::config::merged_config& config ) -> mcode::result< tier_set > {
		auto out = tier_set{ };

		const auto plan = resolve_one( default_model, config, "model.tier.plan" );

		if ( !plan ) {
			return std::unexpected( plan.error( ) );
		}

		const auto act = resolve_one( default_model, config, "model.tier.act" );

		if ( !act ) {
			return std::unexpected( act.error( ) );
		}

		const auto side = resolve_one( default_model, config, "model.tier.side" );

		if ( !side ) {
			return std::unexpected( side.error( ) );
		}

		out.plan = *plan;
		out.act = *act;
		out.side = *side;

		return out;
	}

}
