#pragma once

#include "cloud/detail/provider.hpp"

namespace cloud {
namespace detail {

// GCP transport and public catalogue pricing ---------------------------------

inline gcp::detail::HttpResponse call(
    const client_state& client, const gcp::detail::HttpRequest& request,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    // Status-less failures plus HTTP 408, 429, and 5xx are retried. Mutation
    // callers also use provider idempotency keys for ambiguous responses.
    const auto base_timeout = request.timeout.value_or(client.config.request_timeout);
    for (int attempt = 0;; ++attempt) {
        auto current = request;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw error("Cloud operation deadline exceeded");
            current.timeout = std::max(
                std::chrono::milliseconds(1),
                std::min(base_timeout,
                         std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        try {
            return client.core()->call(std::move(current));
        } catch (const error& failure) {
            if (attempt == 3 || !retryable(failure.http_status()))
                throw;
        }
        auto backoff = std::chrono::milliseconds(100 * (1 << attempt));
        if (deadline != std::chrono::steady_clock::time_point::max())
            backoff =
                std::min(backoff, std::max(std::chrono::milliseconds::zero(),
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - std::chrono::steady_clock::now())));
        if (backoff <= std::chrono::milliseconds::zero())
            throw error("Cloud operation deadline exceeded");
        std::this_thread::sleep_for(backoff);
    }
}

inline gcp::detail::HttpResponse public_call(const client_state& client,
                                             gcp::detail::HttpRequest request) {
    validate_endpoint(client, request.url, "Public pricing endpoint");
    const auto base_timeout = request.timeout.value_or(client.config.request_timeout);
    for (int attempt = 0;; ++attempt) {
        request.timeout = base_timeout;
        try {
            auto response = gcp::detail::http(request);
            check_response("Public pricing API", response);
            return response;
        } catch (const error& failure) {
            if (attempt == 3 || !retryable(failure.http_status()))
                throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
    }
}

inline double decimal(std::string text, std::string_view field_name) {
    if (text.empty())
        throw error("Missing decimal " + std::string(field_name));
    const auto value = parse_decimal(text);
    if (!value || *value < 0)
        throw error("Invalid decimal " + std::string(field_name));
    return *value;
}

inline std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), gcp::detail::ascii_lower);
    return value;
}

inline bool contains(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

inline bool json_array_contains(const gcp::detail::Json& value, std::string_view key,
                                std::string_view wanted) {
    const auto* array = value.get(key);
    if (!array)
        return false;
    return std::any_of(array->array().begin(), array->array().end(),
                       [&](const auto& item) { return item.text() == wanted; });
}

inline std::optional<double> gcp_sku_unit_price(const gcp::detail::Json& sku) {
    const auto* info = sku.get("pricingInfo");
    if (!info || info->array().empty())
        return std::nullopt;
    const auto& latest = info->array().back();
    const auto* expression = latest.get("pricingExpression");
    const auto* rates = expression ? expression->get("tieredRates") : nullptr;
    if (!rates || rates->array().empty())
        return std::nullopt;
    const auto* price = rates->array().front().get("unitPrice");
    if (!price || gcp::detail::field(*price, "currencyCode") != "USD")
        return std::nullopt;
    const std::string units = gcp::detail::field(*price, "units");
    const std::string nanos = gcp::detail::field(*price, "nanos");
    const double whole = units.empty() ? 0.0 : decimal(units, "GCP price units");
    const double fractional = nanos.empty() ? 0.0 : decimal(nanos, "GCP price nanos") / 1e9;
    return whole + fractional;
}

inline std::pair<double, double> gcp_machine_resources(const cloud::plan& chosen) {
    const auto suffix = [&](std::string_view marker) -> std::optional<unsigned> {
        if (!gcp::detail::starts_with(chosen.machine_type, marker))
            return std::nullopt;
        try {
            return static_cast<unsigned>(std::stoul(chosen.machine_type.substr(marker.size())));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    if (const auto n = suffix("e2-standard-"))
        return {*n, *n * 4.0};
    if (const auto n = suffix("n1-standard-"))
        return {*n, *n * 3.75};
    if (const auto n = suffix("g2-standard-"))
        return {*n, *n * 4.0};
    if (gcp::detail::starts_with(chosen.machine_type, "a2-ultragpu-"))
        return {12.0 * chosen.accelerator_count, 170.0 * chosen.accelerator_count};
    if (gcp::detail::starts_with(chosen.machine_type, "a3-highgpu-"))
        return {26.0 * chosen.accelerator_count, 234.0 * chosen.accelerator_count};
    throw error("No GCP pricing composition for machine type " + chosen.machine_type);
}

inline std::optional<double> gcp_catalogue_price(const client_state& client,
                                                const cloud::plan& chosen, bool spot) {
    // Compose hourly CPU, RAM, and optional GPU component SKUs. Filters reject
    // custom/sole-tenant/commitment variants and conflicting component matches.
    // A2 Ultra and A3 High shapes also carry mandatory billed Local SSD. Until
    // that inseparable component can be quoted on the same hourly basis, leave
    // the whole estimate unavailable so a configured price ceiling fails closed.
    if (gcp::detail::starts_with(chosen.machine_type, "a2-ultragpu-") ||
        gcp::detail::starts_with(chosen.machine_type, "a3-highgpu-"))
        return std::nullopt;
    const std::string endpoint = gcp::detail::base_url(client.config.billing_endpoint);
    validate_endpoint(client, endpoint, "GCP billing_endpoint");
    struct component {
        std::vector<std::string> needles;
        std::string label;
        double quantity;
        std::optional<double> unit;
    };
    const auto [cpus, memory] = gcp_machine_resources(chosen);
    std::string family = chosen.machine_type.substr(0, chosen.machine_type.find('-'));
    std::vector<component> components{
        {{family, "instance core"}, family + " core", cpus, std::nullopt},
        {{family, "instance ram"}, family + " ram", memory, std::nullopt}};
    if (!chosen.accelerator.empty())
        components.push_back({{chosen.accelerator, "gpu"},
                              chosen.accelerator + " gpu",
                              static_cast<double>(chosen.accelerator_count),
                              std::nullopt});

    std::string page;
    std::unordered_set<std::string> seen_pages;
    std::size_t response_bytes = 0;
    std::size_t pages = 0;
    constexpr std::size_t max_catalogue_response_bytes = 256 * 1024 * 1024;
    do {
        // The byte budget bounds ordinary catalogues; this independent ceiling
        // also bounds a hostile sequence of tiny pages with unique tokens.
        if (pages == 1024)
            throw error("GCP catalogue pagination exceeded 1,024 pages");
        ++pages;
        // Small pages stay comfortably within the private response/tree limits,
        // even when individual SKU descriptions contain many pricing tiers.
        std::string url = endpoint + "/v1/services/6F81-5844-456A/skus?pageSize=200";
        if (!page.empty())
            url += "&pageToken=" + gcp::detail::encode(page);
        const auto response =
            call(client, gcp::detail::HttpRequest{}.with_url(std::move(url)));
        if (response.body.size() >
            max_catalogue_response_bytes -
                std::min(response_bytes, max_catalogue_response_bytes))
            throw error("GCP catalogue exceeded the aggregate response byte limit");
        response_bytes += response.body.size();
        const auto json = gcp::detail::parse_json(response.body);
        gcp::detail::for_each_json(json, "skus", [&](const gcp::detail::Json& sku) {
            if (!json_array_contains(sku, "serviceRegions", chosen.region))
                return;
            const std::string description = lowercase(gcp::detail::field(sku, "description"));
            const auto* category = sku.get("category");
            const std::string usage =
                category ? lowercase(gcp::detail::field(*category, "usageType")) : std::string{};
            if (spot ? (usage != "preemptible" && usage != "spot") : usage != "ondemand")
                return;
            if ((family == "e2" || family == "g2") && contains(description, "custom"))
                return;
            if (contains(description, "sole tenancy"))
                return;
            if (family == "n1" &&
                (contains(description, "instance core") || contains(description, "instance ram")) &&
                !contains(description, "predefined"))
                return;
            if ((chosen.accelerator == "a100" || chosen.accelerator == "h100") &&
                contains(description, chosen.accelerator) && contains(description, "gpu") &&
                !contains(description, "80gb"))
                return;
            if (family == "a3" && contains(description, "mega"))
                return;
            for (auto& component : components) {
                if (!std::all_of(component.needles.begin(), component.needles.end(),
                                 [&](const auto& needle) { return contains(description, needle); }))
                    continue;
                const auto price = gcp_sku_unit_price(sku);
                if (!price)
                    continue;
                if (component.unit && std::fabs(*component.unit - *price) > 1e-12)
                    throw error("GCP catalogue returned ambiguous prices for " + component.label);
                component.unit = price;
            }
        });
        const std::string next = gcp::detail::field(json, "nextPageToken");
        if (next.empty()) {
            page.clear();
            break;
        }
        if (!seen_pages.insert(next).second)
            throw error("GCP catalogue pagination repeated a page token");
        page = next;
    } while (!page.empty());

    double total = 0;
    for (const auto& component : components) {
        if (!component.unit)
            return std::nullopt;
        total += *component.unit * component.quantity;
    }
    return total;
}

inline std::optional<double> aws_catalogue_price(const client_state& client,
                                                const cloud::plan& chosen, bool spot) {
    // Spot is the newest Linux/UNIX observation for one exact Availability Zone.
    // On-demand requires one unique Linux/shared/no-software hourly price term.
    if (chosen.machine_type == "batch-managed" || chosen.machine_type == "FARGATE")
        return std::nullopt;
    if (spot) {
        const std::string zone = configured_zone(client.config, "aws");
        if (zone.empty())
            return std::nullopt;
        const std::string endpoint =
            aws_endpoint(client, client.config.aws.ec2_endpoint, "ec2", chosen.region);
        const std::string body =
            "Action=DescribeSpotPriceHistory&Version=2016-11-15&MaxResults=1&InstanceType.1=" +
            gcp::detail::encode(chosen.machine_type) +
            "&ProductDescription.1=Linux%2FUNIX&AvailabilityZone=" + gcp::detail::encode(zone) +
            "&StartTime=" + gcp::detail::encode(iso_time());
        const auto response =
            aws_call(client,
                     gcp::detail::HttpRequest{}
                         .with_method("POST")
                         .with_url(endpoint + '/')
                         .with_headers({"Content-Type: application/x-www-form-urlencoded"})
                         .with_body(body)
                         .with_accept_json(false),
                     chosen.region, "ec2");
        const std::string open = "<spotPrice>";
        const std::string close = "</spotPrice>";
        const auto begin = response.body.find(open);
        const auto end = begin == std::string::npos
                             ? std::string::npos
                             : response.body.find(close, begin + open.size());
        if (begin == std::string::npos || end == std::string::npos)
            return std::nullopt;
        return decimal(response.body.substr(begin + open.size(), end - begin - open.size()),
                       "AWS Spot price");
    }

    const std::string endpoint = gcp::detail::base_url(client.config.aws.pricing_endpoint);
    validate_endpoint(client, endpoint, "AWS pricing_endpoint");
    const auto filter = [](std::string_view field, std::string_view value) {
        return "{\"Type\":\"TERM_MATCH\",\"Field\":" + gcp::detail::json_quote(field) +
               ",\"Value\":" + gcp::detail::json_quote(value) + '}';
    };
    std::optional<double> found;
    std::string token;
    do {
        std::string body = "{\"ServiceCode\":\"AmazonEC2\",\"FormatVersion\":\"aws_v1\","
                           "\"MaxResults\":100,\"Filters\":[" +
                           filter("instanceType", chosen.machine_type) + ',' +
                           filter("regionCode", chosen.region) + ',' +
                           filter("operatingSystem", "Linux") + ',' + filter("tenancy", "Shared") +
                           ',' + filter("preInstalledSw", "NA") + ',' +
                           filter("capacitystatus", "Used") + ']';
        if (!token.empty())
            body += ",\"NextToken\":" + gcp::detail::json_quote(token);
        body += '}';
        const auto outer = gcp::detail::parse_json(
            aws_call(client,
                     gcp::detail::HttpRequest{}
                         .with_method("POST")
                         .with_url(endpoint + '/')
                         .with_headers({"Content-Type: application/x-amz-json-1.1",
                                        "X-Amz-Target: AWSPriceListService.GetProducts"})
                         .with_body(body),
                     client.config.aws.pricing_region, "pricing")
                .body);
        const auto* products = outer.get("PriceList");
        if (products)
            for (const auto& encoded_product : products->array()) {
                const auto product = gcp::detail::parse_json(encoded_product.text());
                const auto* terms = product.get("terms");
                const auto* on_demand = terms ? terms->get("OnDemand") : nullptr;
                if (!on_demand)
                    continue;
                for (const auto& [unused_term, term] : on_demand->object()) {
                    (void)unused_term;
                    const auto* dimensions = term.get("priceDimensions");
                    if (!dimensions)
                        continue;
                    for (const auto& [unused_dimension, dimension] : dimensions->object()) {
                        (void)unused_dimension;
                        if (gcp::detail::field(dimension, "unit") != "Hrs")
                            continue;
                        const auto* per_unit = dimension.get("pricePerUnit");
                        if (!per_unit)
                            continue;
                        const std::string usd = gcp::detail::field(*per_unit, "USD");
                        if (usd.empty())
                            continue;
                        const double price = decimal(usd, "AWS on-demand price");
                        if (found && std::fabs(*found - price) > 1e-12)
                            throw error("AWS catalogue returned ambiguous hourly prices");
                        found = price;
                    }
                }
            }
        token = gcp::detail::field(outer, "NextToken");
    } while (!token.empty());
    return found;
}

inline std::optional<double> azure_catalogue_price(const client_state& client,
                                                  const cloud::plan& chosen, bool spot) {
    // Select the latest effective primary-region USD consumption row. Windows,
    // low-priority, wrong-market, and conflicting same-date rows are rejected.
    const std::string endpoint = gcp::detail::base_url(client.config.azure.pricing_endpoint);
    validate_endpoint(client, endpoint, "Azure pricing_endpoint");
    const std::string pricing_origin = endpoint_origin(endpoint);
    const std::string filter = "serviceName eq 'Virtual Machines' and armRegionName eq '" +
                               chosen.region + "' and armSkuName eq '" + chosen.machine_type +
                               "' and priceType eq 'Consumption'";
    std::string url = endpoint + "?$filter=" + gcp::detail::encode(filter);
    std::optional<double> found;
    std::string effective;
    const std::string now = iso_time();
    while (!url.empty()) {
        if (endpoint_origin(url) != pricing_origin)
            throw error("Azure retail price pagination changed origin");
        const auto json = gcp::detail::parse_json(
            public_call(client, gcp::detail::HttpRequest{}.with_url(url)).body);
        gcp::detail::for_each_json(json, "Items", [&](const gcp::detail::Json& item) {
            const std::string arm_sku = gcp::detail::field(item, "armSkuName");
            if (gcp::detail::field(item, "currencyCode") != "USD" ||
                gcp::detail::field(item, "armRegionName") != chosen.region ||
                (arm_sku != chosen.machine_type &&
                 (!spot || arm_sku != chosen.machine_type + " Spot")) ||
                gcp::detail::field(item, "type") != "Consumption" ||
                gcp::detail::field(item, "unitOfMeasure") != "1 Hour" ||
                !item.get("isPrimaryMeterRegion") || !item.get("isPrimaryMeterRegion")->boolean())
                return;
            const std::string product = lowercase(gcp::detail::field(item, "productName"));
            const std::string meter = lowercase(gcp::detail::field(item, "meterName"));
            const std::string sku = lowercase(gcp::detail::field(item, "skuName"));
            if (contains(product, "windows"))
                return;
            if (contains(meter, "low priority") || contains(sku, "low priority"))
                return;
            const bool item_spot = contains(meter, "spot") || contains(sku, "spot");
            if (item_spot != spot)
                return;
            const std::string item_effective = gcp::detail::field(item, "effectiveStartDate");
            if (item_effective.empty() || item_effective > now)
                return;
            const std::string value = gcp::detail::field(item, "retailPrice");
            if (value.empty())
                return;
            const double price = decimal(value, "Azure retail price");
            if (effective.empty() || item_effective > effective) {
                effective = item_effective;
                found = price;
            } else if (item_effective == effective && found && std::fabs(*found - price) > 1e-12) {
                throw error(
                    "Azure retail API returned conflicting prices with the same effective date");
            }
        });
        url = gcp::detail::field(json, "NextPageLink");
    }
    return found;
}

inline std::optional<double> catalogue_price(const client_state& client,
                                             const cloud::plan& chosen, bool spot) {
    // Cache keys include provider, region, zone, native shape, accelerator, and
    // market. Only successful quotes are cached; unavailable prices are retried.
    const std::string key = chosen.provider + '\n' + chosen.region + '\n' +
                            configured_zone(client.config, chosen.provider) + '\n' +
                            chosen.machine_type + '\n' + chosen.accelerator + '\n' +
                            std::to_string(chosen.accelerator_count) +
                            (spot ? "\nspot" : "\nondemand");
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = spot ? client.config.spot_price_cache_ttl : client.config.price_cache_ttl;
    {
        std::lock_guard lock(client.price_mutex);
        const auto it = client.price_cache.find(key);
        if (it != client.price_cache.end() && now - it->second.first < ttl)
            return it->second.second;
    }
    std::optional<double> price;
    if (chosen.provider == "gcp")
        price = gcp_catalogue_price(client, chosen, spot);
    if (chosen.provider == "aws")
        price = aws_catalogue_price(client, chosen, spot);
    if (chosen.provider == "azure")
        price = azure_catalogue_price(client, chosen, spot);
    if (price) {
        std::lock_guard lock(client.price_mutex);
        client.price_cache[key] = {now, *price};
    }
    return price;
}

inline cloud::plan priced_plan(const client_state& client, const job_spec& spec,
                               std::string provider) {
    // Enforce a single pricing source and fail closed for a configured ceiling.
    // The compatibility callback cannot price accelerators because its signature
    // predates model/count fields, so those plans require the rich callback.
    auto out = make_provider_plan(client.config, spec, std::move(provider));
    const bool custom = static_cast<bool>(client.config.lookup_hourly_cost) ||
                        static_cast<bool>(client.config.estimate_hourly_cost);
    if (client.config.lookup_hourly_cost) {
        price_request request;
        request.provider = out.provider;
        request.region = out.region;
        request.zone = configured_zone(client.config, out.provider);
        request.machine_type = out.machine_type;
        request.accelerator = out.accelerator;
        request.accelerator_count = out.accelerator_count;
        request.spot = spec.resources.spot;
        request.cpus = spec.resources.cpus;
        request.memory_gb =
            out.provider == "aws" && !spec.mounts.empty()
                ? static_cast<double>(fargate_memory_mib(spec.resources)) / 1024.0
                : spec.resources.memory_gb;
        out.estimated_hourly_cost = client.config.lookup_hourly_cost(request);
    } else if (client.config.estimate_hourly_cost) {
        if (!out.accelerator.empty())
            throw error(
                "Accelerator pricing requires config::lookup_hourly_cost; the compatibility "
                "estimator omits the accelerator and count");
        out.estimated_hourly_cost = client.config.estimate_hourly_cost(
            out.provider, out.region, out.machine_type, spec.resources.spot);
    } else if (client.config.prices == price_source::public_catalogue) {
        out.estimated_hourly_cost = catalogue_price(client, out, spec.resources.spot);
    }
    if (out.estimated_hourly_cost &&
        (!std::isfinite(*out.estimated_hourly_cost) || *out.estimated_hourly_cost < 0))
        throw error("Price lookup returned an invalid hourly price");
    if (!out.estimated_hourly_cost)
        out.warnings.push_back("hourly cost unavailable; estimates are never guarantees");
    else if (custom)
        out.warnings.push_back(
            "hourly estimate came from the caller-supplied callback and remains advisory");
    else
        out.warnings.push_back("hourly estimate is public USD compute list price; disks, network, "
                               "taxes, and discounts are excluded");
    if (spec.resources.max_price_per_hour) {
        if (!out.estimated_hourly_cost)
            throw error("A maximum hourly price requires an available price lookup");
        const double estimate = *out.estimated_hourly_cost;
        const double maximum = *spec.resources.max_price_per_hour;
        const double roundoff = 8 * std::numeric_limits<double>::epsilon() *
                                std::max(std::fabs(estimate), std::fabs(maximum));
        if (estimate > maximum && estimate - maximum > roundoff)
            throw error("Estimated hourly price exceeds the configured maximum");
    }
    return out;
}

inline cloud::plan make_plan(const client_state& client, const job_spec& spec) {
    // ordered tolerates an unrunnable provider and records why it was skipped.
    // lowest_cost instead requires comparable successful quotes before choosing;
    // stable provider order breaks equal-price ties.
    const std::vector<provider> choices = client.config.provider
                                              ? std::vector<provider>{*client.config.provider}
                                              : client.config.providers;
    if (choices.empty())
        throw error("No cloud provider configured");
    std::vector<cloud::plan> candidates;
    std::vector<std::string> skipped;
    for (const auto& provider : choices) {
        if (!implemented(provider)) {
            skipped.push_back(provider + " backend is not implemented; skipped");
            if (client.config.provider)
                throw error(provider + " backend is not implemented");
            continue;
        }
        try {
            auto candidate = priced_plan(client, spec, provider);
            candidate.warnings.insert(candidate.warnings.begin(), skipped.begin(), skipped.end());
            if (client.config.selection == selection::ordered)
                return candidate;
            if (!candidate.estimated_hourly_cost)
                throw error("lowest_cost requires an hourly price for " + provider);
            candidates.push_back(std::move(candidate));
        } catch (const error& failure) {
            if (client.config.provider || client.config.selection == selection::lowest_cost)
                throw;
            skipped.push_back(provider + " skipped: " + failure.what());
        }
    }
    if (candidates.empty())
        throw error("None of the configured cloud providers can run the job");
    if (client.config.selection == selection::lowest_cost && candidates.size() < 2)
        throw error("lowest_cost requires at least two runnable providers with hourly prices");
    const auto best =
        std::min_element(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return *a.estimated_hourly_cost < *b.estimated_hourly_cost;
        });
    return *best;
}

} // namespace detail
} // namespace cloud
