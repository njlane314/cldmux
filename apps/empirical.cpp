#include <cldmux>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view history_version = "1";

struct options {
    std::string workload;
    std::vector<std::string> candidates;
    std::optional<std::string> provider_override;
    std::filesystem::path history_path = ".cldmux-empirical-history";
    std::chrono::milliseconds expected_elapsed{std::chrono::minutes(5)};
    std::uint64_t minimum_observations = 3;
    std::map<std::string, double, std::less<>> hourly_quotes;
    std::map<std::string, double, std::less<>> data_costs;
    bool no_catalogue = false;
    bool submit = false;
    bool help = false;
};

struct observation {
    std::string workload;
    std::string provider;
    std::string region;
    std::string machine;
    std::string accelerator;
    std::uint64_t accelerator_count = 0;
    bool spot = false;
    std::uint64_t requested_cpus = 0;
    double requested_memory_gb = 0;
    double observed_elapsed_seconds = 0;
    bool succeeded = false;
    std::uint64_t recorded_at_unix_seconds = 0;
};

struct runtime_statistics {
    std::uint64_t count = 0;
    double total_seconds = 0;

    [[nodiscard]] double mean_seconds() const {
        if (!count)
            throw std::logic_error("cannot average an empty observation set");
        return total_seconds / static_cast<double>(count);
    }
};

struct candidate {
    std::string provider;
    cldmux::client client;
    cldmux::plan plan;
    double hourly_quote_usd = 0;
    bool quote_overridden = false;
    double known_data_cost_usd = 0;
    runtime_statistics history;
    double runtime_seconds = 0;
    bool runtime_from_history = false;
    double effective_cost_proxy_usd = 0;
};

struct routing_decision {
    std::vector<candidate> candidates;
    std::size_t selected = 0;
    std::string basis;
};

bool known_provider(std::string_view value) {
    return value == "gcp" || value == "aws" || value == "azure";
}

bool safe_workload(std::string_view value) {
    if (value.empty())
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

bool safe_history_token(std::string_view value) {
    if (value.empty())
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
    });
}

double parse_nonnegative_number(std::string_view text, std::string_view description) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    double value = 0;
    char trailing = '\0';
    if (!(input >> value) || (input >> trailing) || !std::isfinite(value) || value < 0)
        throw std::invalid_argument(std::string(description) +
                                    " must be a finite, non-negative number");
    return value;
}

std::uint64_t parse_unsigned(std::string_view text, std::string_view description,
                             bool permit_zero = true) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (!permit_zero && !value))
        throw std::invalid_argument(std::string(description) + " must be a valid integer");
    return value;
}

std::chrono::milliseconds parse_runtime(std::string_view text) {
    if (text.size() < 2)
        throw std::invalid_argument("expected elapsed time requires a positive s, m, or h suffix");

    const char suffix = text.back();
    std::uint64_t multiplier = 0;
    if (suffix == 's')
        multiplier = 1'000;
    else if (suffix == 'm')
        multiplier = 60'000;
    else if (suffix == 'h')
        multiplier = 3'600'000;
    else
        throw std::invalid_argument("expected elapsed time requires an s, m, or h suffix");

    const std::uint64_t quantity =
        parse_unsigned(text.substr(0, text.size() - 1), "expected elapsed time", false);
    using rep = std::chrono::milliseconds::rep;
    const auto limit = static_cast<std::uint64_t>((std::numeric_limits<rep>::max)());
    if (quantity > limit / multiplier)
        throw std::invalid_argument("expected elapsed time is too large");
    return std::chrono::milliseconds(static_cast<rep>(quantity * multiplier));
}

std::vector<std::string> parse_candidates(std::string_view text) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string_view item =
            text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (!known_provider(item))
            throw std::invalid_argument("candidate must be gcp, aws, or azure");
        if (std::find(result.begin(), result.end(), item) != result.end())
            throw std::invalid_argument("candidate providers must be unique");
        result.emplace_back(item);
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

std::pair<std::string, double> parse_provider_number(std::string_view text,
                                                     std::string_view description) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos || !separator || separator + 1 >= text.size())
        throw std::invalid_argument(std::string(description) + " requires provider:value");
    const std::string_view provider = text.substr(0, separator);
    if (!known_provider(provider))
        throw std::invalid_argument(std::string(description) +
                                    " provider must be gcp, aws, or azure");
    return {std::string(provider),
            parse_nonnegative_number(text.substr(separator + 1), description)};
}

template <class Map>
void insert_unique(Map& values, std::pair<std::string, double> item, std::string_view description) {
    if (!values.emplace(std::move(item)).second)
        throw std::invalid_argument(std::string(description) +
                                    " may be supplied only once per provider");
}

bool option_value(std::string_view argument, std::string_view name, std::string_view& value) {
    if (argument.size() < name.size() || argument.substr(0, name.size()) != name)
        return false;
    value = argument.substr(name.size());
    return true;
}

void set_once(bool& seen, std::string_view name) {
    if (std::exchange(seen, true))
        throw std::invalid_argument(std::string(name) + " may be supplied only once");
}

void print_help(std::ostream& output) {
    output << "Usage: cldmux-empirical WORKLOAD "
              "--candidates=gcp,aws[,azure] [OPTIONS]\n"
           << "\n"
           << "Route by quote snapshot * learned elapsed / 3600 + data-cost snapshot.\n"
           << "WORKLOAD is a stable, author-versioned key such as render-v2.\n"
           << "\n"
           << "Options:\n"
           << "  --provider=aws                  override empirical selection\n"
           << "  --history=PATH                 ledger (default .cldmux-empirical-history)\n"
           << "  --minimum-observations=3       successful samples required per provider\n"
           << "  --expected-elapsed=5m          common cold-start fallback (s, m, h)\n"
           << "  --data-cost=aws:0.02           caller USD data-cost snapshot per run\n"
           << "  --hourly-quote=aws:0.192       caller quote override and test seam\n"
           << "  --no-catalogue                 require quote overrides; perform no lookup\n"
           << "  --submit                       run the selected job and record its elapsed time\n"
           << "  --help                         show this text and exit\n"
           << "\n"
           << "Automatic routing is strict: every named candidate must plan, quote, and have\n"
           << "a known data cost. A missing history cohort makes every candidate use the\n"
           << "same fallback; use --provider to gather deliberate observations.\n";
}

options parse_options(int argc, char* argv[]) {
    options result;
    bool candidates_seen = false;
    bool provider_seen = false;
    bool history_seen = false;
    bool minimum_seen = false;
    bool runtime_seen = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        std::string_view value;
        if (argument == "--help") {
            set_once(result.help, "--help");
        } else if (argument == "--submit") {
            set_once(result.submit, "--submit");
        } else if (argument == "--no-catalogue") {
            set_once(result.no_catalogue, "--no-catalogue");
        } else if (option_value(argument, "--candidates=", value)) {
            set_once(candidates_seen, "--candidates");
            result.candidates = parse_candidates(value);
        } else if (option_value(argument, "--provider=", value)) {
            set_once(provider_seen, "--provider");
            if (!known_provider(value))
                throw std::invalid_argument("provider override must be gcp, aws, or azure");
            result.provider_override = std::string(value);
        } else if (option_value(argument, "--history=", value)) {
            set_once(history_seen, "--history");
            if (value.empty())
                throw std::invalid_argument("--history requires a path");
            result.history_path = std::filesystem::path(std::string(value));
        } else if (option_value(argument, "--minimum-observations=", value)) {
            set_once(minimum_seen, "--minimum-observations");
            result.minimum_observations = parse_unsigned(value, "minimum observations", false);
        } else if (option_value(argument, "--expected-elapsed=", value)) {
            set_once(runtime_seen, "--expected-elapsed");
            result.expected_elapsed = parse_runtime(value);
        } else if (option_value(argument, "--data-cost=", value)) {
            insert_unique(result.data_costs, parse_provider_number(value, "data cost"),
                          "--data-cost");
        } else if (option_value(argument, "--hourly-quote=", value)) {
            insert_unique(result.hourly_quotes, parse_provider_number(value, "hourly quote"),
                          "--hourly-quote");
        } else if (argument.size() >= 2 && argument.substr(0, 2) == "--") {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        } else if (result.workload.empty()) {
            if (!safe_workload(argument))
                throw std::invalid_argument(
                    "workload must contain only letters, digits, dot, underscore, or hyphen");
            result.workload = argument;
        } else {
            throw std::invalid_argument("only one workload key may be supplied");
        }
    }

    if (result.help)
        return result;
    if (result.workload.empty())
        throw std::invalid_argument("a workload key is required; use --help for usage");
    if (!result.provider_override && result.candidates.size() < 2)
        throw std::invalid_argument(
            "automatic routing requires at least two explicit candidate providers");
    return result;
}

std::string field(std::string_view token, std::string_view name) {
    const std::string prefix = std::string(name) + '=';
    if (token.size() < prefix.size() || token.substr(0, prefix.size()) != prefix)
        throw std::invalid_argument("expected history field " + std::string(name));
    return std::string(token.substr(prefix.size()));
}

bool parse_boolean(std::string_view text, std::string_view description) {
    if (text == "true")
        return true;
    if (text == "false")
        return false;
    throw std::invalid_argument(std::string(description) + " must be true or false");
}

std::string format_number(double value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("history numbers must be finite");
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

class history_store {
public:
    explicit history_store(std::filesystem::path path) : path_(std::move(path)) {}

    [[nodiscard]] std::vector<observation> load() const {
        std::ifstream input(path_);
        if (!input) {
            std::error_code failure;
            if (!std::filesystem::exists(path_, failure) && !failure)
                return {};
            throw std::runtime_error("cannot read empirical history: " + path_.string());
        }
        input.imbue(std::locale::classic());

        std::vector<observation> result;
        std::string line;
        std::uint64_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (line.empty() || line.front() == '#')
                continue;
            try {
                result.push_back(parse(line));
            } catch (const std::exception& failure) {
                throw std::runtime_error("invalid empirical history at line " +
                                         std::to_string(line_number) + ": " + failure.what());
            }
        }
        if (!input.eof())
            throw std::runtime_error("cannot finish reading empirical history: " + path_.string());
        return result;
    }

    // Refuse a malformed ledger or unusable destination before a paid job can
    // start. Opening in append mode also creates a requested new ledger, but
    // never truncates an existing one.
    void prepare_append() const {
        (void)load();
        require_final_newline();

        std::ofstream output(path_, std::ios::app);
        if (!output)
            throw std::runtime_error("cannot prepare empirical history for append: " +
                                     path_.string());
        output.flush();
        if (!output)
            throw std::runtime_error("cannot prepare empirical history for append: " +
                                     path_.string());
    }

    void append(const observation& value) const {
        validate(value);
        prepare_append();

        std::ofstream output(path_, std::ios::app);
        if (!output)
            throw std::runtime_error("cannot append empirical history: " + path_.string());
        output.imbue(std::locale::classic());
        output << "history_version=" << history_version << " workload=" << value.workload
               << " provider=" << value.provider << " region=" << value.region
               << " machine=" << value.machine << " accelerator=" << value.accelerator
               << " accelerator_count=" << value.accelerator_count
               << " spot=" << (value.spot ? "true" : "false")
               << " requested_cpus=" << value.requested_cpus
               << " requested_memory_gb=" << format_number(value.requested_memory_gb)
               << " observed_elapsed_seconds=" << format_number(value.observed_elapsed_seconds)
               << " succeeded=" << (value.succeeded ? "true" : "false")
               << " recorded_at_unix_seconds=" << value.recorded_at_unix_seconds << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("cannot finish appending empirical history: " +
                                     path_.string());
    }

private:
    void require_final_newline() const {
        std::ifstream existing(path_, std::ios::binary | std::ios::ate);
        if (!existing)
            return;

        const std::streampos size = existing.tellg();
        if (size == std::streampos(-1))
            throw std::runtime_error("cannot inspect empirical history: " + path_.string());
        if (size == std::streampos(0))
            return;

        existing.seekg(std::streamoff(-1), std::ios::end);
        char last = '\0';
        existing.get(last);
        if (!existing || last != '\n')
            throw std::runtime_error("empirical history must end with a newline before append: " +
                                     path_.string());
    }

    static void validate(const observation& value) {
        if (!safe_workload(value.workload) || !known_provider(value.provider) ||
            !safe_history_token(value.region) || !safe_history_token(value.machine) ||
            !safe_history_token(value.accelerator) || !value.requested_cpus ||
            !std::isfinite(value.requested_memory_gb) || value.requested_memory_gb <= 0 ||
            !std::isfinite(value.observed_elapsed_seconds) || value.observed_elapsed_seconds <= 0)
            throw std::invalid_argument("history record contains an invalid value");
    }

    static observation parse(const std::string& line) {
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        std::vector<std::string> tokens;
        for (std::string token; input >> token;)
            tokens.push_back(std::move(token));
        if (tokens.size() != 13)
            throw std::invalid_argument("history record must contain exactly 13 fields");

        if (field(tokens[0], "history_version") != history_version)
            throw std::invalid_argument("unsupported history version");

        observation result;
        result.workload = field(tokens[1], "workload");
        result.provider = field(tokens[2], "provider");
        result.region = field(tokens[3], "region");
        result.machine = field(tokens[4], "machine");
        result.accelerator = field(tokens[5], "accelerator");
        result.accelerator_count =
            parse_unsigned(field(tokens[6], "accelerator_count"), "accelerator count");
        result.spot = parse_boolean(field(tokens[7], "spot"), "spot");
        result.requested_cpus =
            parse_unsigned(field(tokens[8], "requested_cpus"), "requested CPUs", false);
        result.requested_memory_gb =
            parse_nonnegative_number(field(tokens[9], "requested_memory_gb"), "requested memory");
        result.observed_elapsed_seconds = parse_nonnegative_number(
            field(tokens[10], "observed_elapsed_seconds"), "observed elapsed seconds");
        result.succeeded = parse_boolean(field(tokens[11], "succeeded"), "succeeded");
        result.recorded_at_unix_seconds =
            parse_unsigned(field(tokens[12], "recorded_at_unix_seconds"), "recorded time");

        validate(result);
        return result;
    }

    std::filesystem::path path_;
};

std::string accelerator_name(const cldmux::plan& value) {
    return value.accelerator.empty() ? "none" : value.accelerator;
}

double effective_cost(double hourly_quote, double runtime_seconds, double data_cost) {
    const double result = hourly_quote * runtime_seconds / 3'600.0 + data_cost;
    if (!std::isfinite(result) || result < 0)
        throw std::runtime_error("effective cost proxy is not finite");
    return result;
}

bool same_memory(double left, double right) {
    return std::fabs(left - right) <= 1e-12 * (std::max)({1.0, std::fabs(left), std::fabs(right)});
}

class empirical_router {
public:
    empirical_router(options configuration, cldmux::job_spec job,
                     std::vector<observation> observations)
        : options_(std::move(configuration)), job_(std::move(job)),
          observations_(std::move(observations)) {}

    [[nodiscard]] routing_decision choose() const {
        const std::vector<std::string> providers =
            options_.provider_override ? std::vector<std::string>{*options_.provider_override}
                                       : options_.candidates;
        routing_decision decision;
        for (const auto& provider : providers)
            decision.candidates.push_back(evaluate(provider));

        bool use_history = true;
        for (const auto& value : decision.candidates)
            if (value.history.count < options_.minimum_observations)
                use_history = false;

        const double fallback_seconds =
            std::chrono::duration<double>(options_.expected_elapsed).count();
        for (auto& value : decision.candidates) {
            value.runtime_seconds = use_history ? value.history.mean_seconds() : fallback_seconds;
            value.runtime_from_history = use_history;
            value.effective_cost_proxy_usd = effective_cost(
                value.hourly_quote_usd, value.runtime_seconds, value.known_data_cost_usd);
        }

        if (options_.provider_override) {
            decision.basis = "override";
            return decision;
        }
        if (use_history)
            decision.basis = "empirical";
        else
            decision.basis = "advisory-fallback";

        for (std::size_t index = 1; index < decision.candidates.size(); ++index)
            if (decision.candidates[index].effective_cost_proxy_usd <
                decision.candidates[decision.selected].effective_cost_proxy_usd)
                decision.selected = index;
        return decision;
    }

private:
    [[nodiscard]] candidate evaluate(const std::string& provider) const {
        const auto supplied_quote = options_.hourly_quotes.find(provider);
        if (options_.no_catalogue && supplied_quote == options_.hourly_quotes.end())
            throw std::runtime_error("no hourly quote supplied for candidate " + provider);

        cldmux::router router =
            supplied_quote == options_.hourly_quotes.end()
                ? cldmux::router::from_environment(provider,
                                                   cldmux::price_source::public_catalogue)
                : cldmux::router::from_environment(provider);
        cldmux::plan plan = router.plan(job_);
        cldmux::client client = router.route(plan.provider);
        if (plan.provider != provider)
            throw std::runtime_error("planned provider does not match candidate " + provider);
        if (!safe_history_token(plan.region))
            throw std::runtime_error("candidate " + provider + " region cannot be recorded safely");
        if (!safe_history_token(plan.machine_type))
            throw std::runtime_error("candidate " + provider +
                                     " machine cannot be recorded safely");
        if (!safe_history_token(accelerator_name(plan)))
            throw std::runtime_error("candidate " + provider +
                                     " accelerator cannot be recorded safely");
        if (!job_.resources.cpus || !std::isfinite(job_.resources.memory_gb) ||
            job_.resources.memory_gb <= 0)
            throw std::runtime_error("workload resources cannot be recorded safely");

        double hourly_quote = 0;
        if (supplied_quote != options_.hourly_quotes.end()) {
            hourly_quote = supplied_quote->second;
        } else {
            if (!plan.estimated_hourly_cost)
                throw std::runtime_error("hourly quote unavailable for candidate " + provider);
            hourly_quote = *plan.estimated_hourly_cost;
        }

        const auto supplied_data = options_.data_costs.find(provider);
        if (supplied_data == options_.data_costs.end())
            throw std::runtime_error("known data cost missing for candidate " + provider);

        runtime_statistics statistics;
        for (const auto& item : observations_)
            if (matches(item, provider, plan)) {
                if (statistics.count == (std::numeric_limits<std::uint64_t>::max)())
                    throw std::runtime_error("too many matching empirical observations");
                ++statistics.count;
                statistics.total_seconds += item.observed_elapsed_seconds;
                if (!std::isfinite(statistics.total_seconds))
                    throw std::runtime_error("empirical runtime total is not finite");
            }

        return candidate{provider,
                         std::move(client),
                         std::move(plan),
                         hourly_quote,
                         supplied_quote != options_.hourly_quotes.end(),
                         supplied_data->second,
                         statistics,
                         0,
                         false,
                         0};
    }

    [[nodiscard]] bool matches(const observation& item, const std::string& provider,
                               const cldmux::plan& plan) const {
        return item.succeeded && item.workload == options_.workload && item.provider == provider &&
               item.region == plan.region && item.machine == plan.machine_type &&
               item.accelerator == accelerator_name(plan) &&
               item.accelerator_count == plan.accelerator_count &&
               item.spot == job_.resources.spot &&
               item.requested_cpus == static_cast<std::uint64_t>(job_.resources.cpus) &&
               same_memory(item.requested_memory_gb, job_.resources.memory_gb);
    }

    options options_;
    cldmux::job_spec job_;
    std::vector<observation> observations_;
};

cldmux::command_output diagnostic_output(const options& configuration,
                                        const routing_decision& decision) {
    cldmux::command_output output;
    output.add("output_version", "1");
    output.add("router", "empirical");
    output.add("workload", configuration.workload);
    output.add("requested_provider",
               configuration.provider_override ? *configuration.provider_override : "automatic");
    output.add("history", configuration.history_path.string());
    output.add_unsigned("minimum_observations", configuration.minimum_observations);
    output.add("formula",
               "hourly-quote-snapshot-times-routing-runtime-over-3600-plus-known-data-cost");
    output.add("historical_elapsed_basis", "client-run-through-wait-return");
    output.add("quote_basis", "routing-decision-snapshot");
    output.add("data_cost_basis", "caller-supplied-snapshot");
    output.add("cost_kind", "proxy");

    for (const auto& value : decision.candidates) {
        const std::string prefix = "candidate_" + value.provider + '_';
        output.add(prefix + "region", value.plan.region);
        output.add(prefix + "machine", value.plan.machine_type);
        output.add(prefix + "routing_quote_source",
                   value.quote_overridden ? "caller-override" : "public-catalogue");
        output.add_number(prefix + "routing_hourly_quote_usd", value.hourly_quote_usd);
        output.add_unsigned(prefix + "observations", value.history.count);
        output.add(prefix + "routing_runtime_source", value.runtime_from_history
                                                          ? "historical-controller-wall-mean"
                                                          : "caller-expected-fallback");
        output.add_number(prefix + "routing_runtime_seconds", value.runtime_seconds);
        output.add_number(prefix + "known_data_cost_usd", value.known_data_cost_usd);
        output.add_number(prefix + "effective_cost_proxy_usd", value.effective_cost_proxy_usd);
        for (const auto& warning : value.plan.warnings) {
            if (value.quote_overridden &&
                warning == "hourly cost unavailable; estimates are never guarantees")
                continue;
            output.add("warning", value.provider + ": " + warning);
        }
        if (value.quote_overridden)
            output.add("warning", value.provider +
                                      ": caller-supplied hourly quote is not verified or "
                                      "repriced by cldmux");
    }

    output.add("routing_basis", decision.basis);
    output.add("provider", decision.candidates[decision.selected].provider);
    output.add("warning",
               "historical elapsed spans client.run() through wait() return and includes "
               "submission, polling, queueing, provider retries, log collection, and cleanup "
               "attempts; it is not provider-billable runtime");
    output.add("warning",
               "provider costs can continue after wait() if cancellation or cleanup fails");
    output.add("warning", "controller timeout requires a live caller inside wait()");
    output.add("warning", "the recovery job_id is flushed within the measured interval");
    output.add("warning", "effective cost is a routing proxy, not a quote or invoice");
    if (decision.basis == "advisory-fallback")
        output.add("warning",
                   "at least one candidate lacks the required matching observations; all "
                   "candidates use the expected elapsed time");
    output.add("preflight", "planned");
    return output;
}

observation make_observation(const options& configuration, const cldmux::job_spec& job,
                             const candidate& selected, double elapsed_seconds, bool succeeded) {
    const auto unix_time = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    if (unix_time < 0)
        throw std::runtime_error("system clock precedes the Unix epoch");
    return observation{configuration.workload,
                       selected.provider,
                       selected.plan.region,
                       selected.plan.machine_type,
                       accelerator_name(selected.plan),
                       selected.plan.accelerator_count,
                       job.resources.spot,
                       static_cast<std::uint64_t>(job.resources.cpus),
                       job.resources.memory_gb,
                       elapsed_seconds,
                       succeeded,
                       static_cast<std::uint64_t>(unix_time)};
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const options configuration = parse_options(argc, argv);
        if (configuration.help) {
            print_help(std::cout);
            return 0;
        }

        // This is application-owned workload policy. Change the workload key
        // whenever image, command, inputs, or other runtime-relevant behaviour
        // changes; native plan fields also prevent observations crossing shapes.
        cldmux::job_spec job;
        job.name = "empirical-job";
        job.image = "ubuntu:24.04";
        job.command = {"/bin/echo", "hello"};
        job.resources.cpus = 4;
        job.resources.memory_gb = 16;
        job.retries = 1;
        job.timeout = std::chrono::minutes(15);
        job.auto_delete = true;

        const history_store history(configuration.history_path);
        empirical_router router(configuration, job, history.load());
        routing_decision decision = router.choose();
        diagnostic_output(configuration, decision).write(std::cout);

        if (!configuration.submit) {
            cldmux::write_command_record(std::cout, "status", "dry-run");
            return 0;
        }

        const candidate& selected = decision.candidates[decision.selected];
        history.prepare_append();
        cldmux::result result;
        std::optional<cldmux::job> submitted;
        double elapsed_seconds = 0;
        try {
            cldmux::write_command_record(std::cout, "status", "submitting");
            std::cout.flush();
            const auto started = std::chrono::steady_clock::now();
            submitted.emplace(selected.client.run(job));
            cldmux::write_command_record(std::cout, "job_id", submitted->id());
            std::cout.flush();
            result = submitted->wait();
            elapsed_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        } catch (...) {
            if (submitted)
                cldmux::write_command_record(
                    std::cout, "warning",
                    "a terminal result was not received; cancellation and cleanup are "
                    "best effort");
            cldmux::write_command_record(std::cout, "status", "failed");
            throw;
        }

        // Once a terminal result exists, local diagnostics and ledger failures
        // must not change that cldmux result or encourage a duplicate submission.
        try {
            for (const auto& line : submitted->logs())
                cldmux::write_command_record(std::cout, "log", line.text);
            cldmux::write_command_record(std::cout, "observed_elapsed_seconds",
                                        format_number(elapsed_seconds));
            cldmux::write_command_record(
                std::cout, "observed_routing_cost_proxy_usd",
                format_number(effective_cost(selected.hourly_quote_usd, elapsed_seconds,
                                             selected.known_data_cost_usd)));

            history.append(
                make_observation(configuration, job, selected, elapsed_seconds, result.success()));
            cldmux::write_command_record(std::cout, "history_recorded", "true");
        } catch (const std::exception& failure) {
            cldmux::write_command_record(std::cout, "history_recorded", "false");
            cldmux::write_command_record(std::cout, "warning",
                                        std::string("empirical observation was not recorded: ") +
                                            failure.what());
        }

        cldmux::command_output::job_result(result).write(std::cout);
        if (!result.success()) {
            cldmux::write_command_record(std::cerr, "error", result.error());
            cldmux::write_command_record(std::cout, "status", "failed");
            return 1;
        }
        cldmux::write_command_record(std::cout, "status", "succeeded");
        return 0;
    } catch (const std::exception& failure) {
        cldmux::write_command_record(std::cerr, "error", failure.what());
        return 2;
    }
}
