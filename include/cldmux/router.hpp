#pragma once

#include "cldmux/client.hpp"

namespace cldmux {

// A router owns provider configuration and makes read-only routing decisions.
// route() returns a client pinned to the selected backend; the router itself
// cannot submit jobs or access provider-owned storage and compute resources.
class router {
public:
    explicit router(cldmux::config value = {})
        : state_(std::make_shared<detail::client_state>(std::move(value))) {}

    // Construct one provider-neutral router from the documented CLDMUX_*
    // environment contract. "cheapest" compares every configured provider;
    // an explicit provider name is the local override.
    [[nodiscard]] static router from_environment(std::string_view provider_name) {
        return router(detail::config_from_environment(provider_name));
    }

    // Explicitly opt an environment-built router into or out of public
    // catalogue pricing. "cheapest" requires comparable prices and cannot opt
    // out; explicit providers remain local and unpriced by default.
    [[nodiscard]] static router from_environment(std::string_view provider_name,
                                                 cldmux::price_source prices) {
        if (provider_name == "cheapest" && prices == cldmux::price_source::none)
            throw error("cheapest environment routing requires public catalogue pricing");
        auto value = detail::config_from_environment(provider_name);
        value.prices = prices;
        return router(std::move(value));
    }

    [[nodiscard]] cldmux::plan plan(const job_spec& spec) const {
        // Does not mutate provider resources; it may invoke a caller pricing
        // callback or query a read-only public catalogue API.
        return detail::make_plan(*state_, spec);
    }

    [[nodiscard]] cldmux::run_diagnostics
    diagnose(const job_spec& spec, std::chrono::milliseconds expected_attempt_runtime) const {
        client::validate_diagnostic_request(spec, expected_attempt_runtime);
        cldmux::plan selected = plan(spec);
        client bound(state_, selected.provider);
        return bound.diagnose_with_plan(spec, expected_attempt_runtime, std::move(selected));
    }

    // Plan once to choose a provider, then pin every subsequent operation to
    // that backend. client::run() still revalidates and reprices that route.
    [[nodiscard]] client route(const job_spec& spec) const {
        return client(state_, plan(spec).provider);
    }

    // Explicit routing is the local override for configured provider state.
    [[nodiscard]] client route(cldmux::provider value) const {
        if (!detail::implemented(value))
            throw error(value + " backend is not implemented");
        if (!detail::configured_provider(state_->config, value))
            throw error(value + " is not configured on this router");
        return client(state_, std::move(value));
    }

    [[nodiscard]] bool supports(std::string_view value, feature requested) const {
        // Capability reports implemented behaviour, not a live account, quota,
        // queue, regional SKU, or credential probe.
        return detail::supports(value, requested);
    }

    [[nodiscard]] bool supports(feature requested) const {
        if (state_->config.provider)
            return supports(*state_->config.provider, requested);
        if (state_->config.providers.empty())
            return false;
        return std::all_of(state_->config.providers.begin(), state_->config.providers.end(),
                           [&](const auto& value) { return supports(value, requested); });
    }

private:
    std::shared_ptr<detail::client_state> state_;
};

} // namespace cldmux
