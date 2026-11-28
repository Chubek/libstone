// Statistics contracts: estimators, bootstrap determinism, comparisons.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "mahak/Statistics.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

Observation observation(double value, bool valid = true) {
    Observation result;
    result.metric = "wall";
    result.unit = "ns";
    result.direction = MetricDirection::LowerIsBetter;
    result.value = value;
    result.valid = valid;
    result.validity = valid ? Validity::Valid : Validity::MeasurementFailed;
    return result;
}

std::vector<Observation> series(std::initializer_list<double> values) {
    std::vector<Observation> result;
    std::uint64_t sequence = 0;
    for (double value : values) {
        Observation entry = observation(value);
        entry.sequence = sequence++;
        result.push_back(entry);
    }
    return result;
}

} // namespace

int main() {
    const std::vector<double> sorted = {1.0, 2.0, 3.0, 4.0};
    check(std::fabs(quantile(sorted, 0.0) - 1.0) < 1e-12, "quantile at 0");
    check(std::fabs(quantile(sorted, 1.0) - 4.0) < 1e-12, "quantile at 1");
    check(std::fabs(quantile(sorted, 0.5) - 2.5) < 1e-12, "type-7 median interpolates");
    check(std::fabs(quantile(sorted, 0.25) - 1.75) < 1e-12, "type-7 q1 interpolates");

    std::vector<Observation> mixed = series({10.0, 20.0, 30.0});
    mixed.push_back(observation(999.0, false));
    const Estimates estimates = estimate(mixed);
    check(estimates.total == 4, "invalid observations counted in total");
    check(estimates.valid == 3, "invalid observations excluded from estimates");
    check(std::fabs(estimates.mean - 20.0) < 1e-12, "mean over valid values");
    check(std::fabs(estimates.median - 20.0) < 1e-12, "median over valid values");
    check(std::fabs(estimates.variance - 100.0) < 1e-9, "sample variance uses n-1");
    check(std::fabs(estimates.mad - 10.0) < 1e-12, "median absolute deviation");

    const BootstrapOptions bootstrap{42, 500, 0.95, 10000};
    const std::vector<double> values = {10.0, 12.0, 11.0, 13.0, 9.0};
    const ConfidenceInterval first = bootstrap_confidence_interval(values, bootstrap);
    const ConfidenceInterval second = bootstrap_confidence_interval(values, bootstrap);
    check(first.defined && second.defined, "bootstrap interval defined");
    check(std::fabs(first.lower - second.lower) < 1e-12 &&
              std::fabs(first.upper - second.upper) < 1e-12,
          "bootstrap is deterministic for a fixed seed");
    check(first.lower <= first.upper, "interval bounds ordered");
    const ConfidenceInterval undefined = bootstrap_confidence_interval({5.0}, bootstrap);
    check(!undefined.defined && undefined.method == "unavailable",
          "single sample yields an undefined interval");

    const std::vector<Observation> with_outlier = series({10.0, 10.0, 10.0, 1000.0});
    OutlierOptions outlier_options;
    const OutlierReport outliers = detect_outliers(with_outlier, outlier_options);
    check(outliers.excluded.size() == 1, "MAD flags the extreme sample");
    check(outliers.retained.size() == 3, "retained samples stay available");
    const OutlierOptions none;
    OutlierOptions disabled = none;
    disabled.method = OutlierOptions::Method::None;
    check(detect_outliers(with_outlier, disabled).excluded.empty(),
          "Method::None disables exclusion");

    ComparisonOptions comparison_options;
    comparison_options.criteria.minimum_samples = 3;
    auto insufficient = compare_observations(series({10.0}), series({9.0}), comparison_options);
    check(static_cast<bool>(insufficient), "insufficient comparison still returns a result");
    if (insufficient)
        check(insufficient.value().result.verdict == "insufficient-data",
              "insufficient data is a verdict, not a regression");

    auto improvement =
        compare_observations(series({100.0, 101.0, 99.0}), series({50.0, 51.0, 49.0}),
                             comparison_options);
    check(static_cast<bool>(improvement), "improvement comparison succeeds");
    if (improvement) {
        check(improvement.value().result.improvement, "lower candidate is an improvement");
        check(!improvement.value().result.regression, "improvement is not a regression");
        check(improvement.value().result.ratio < 1.0, "ratio below one");
        check(improvement.value().result.speedup > 1.0, "speedup above one for latency");
    }

    auto regression =
        compare_observations(series({50.0, 51.0, 49.0}), series({100.0, 101.0, 99.0}),
                             comparison_options);
    check(regression && regression.value().result.regression, "higher candidate regresses");

    auto incompatible = compare_observations(series({1.0, 2.0, 3.0}), [] {
        std::vector<Observation> other = series({1.0, 2.0, 3.0});
        for (auto &entry : other)
            entry.unit = "us";
        return other;
    }(), comparison_options);
    check(!incompatible, "unit mismatch is an error");

    const ScalabilityFit fit = fit_scalability({1.0, 2.0, 4.0}, {1.0, 2.0, 4.0});
    check(fit.defined, "log-log fit defined");
    check(std::fabs(fit.exponent - 1.0) < 1e-9, "linear scaling has exponent 1");
    check(std::fabs(fit.intercept - 1.0) < 1e-9, "linear scaling has unit intercept");
    check(std::fabs(fit.r_squared - 1.0) < 1e-9, "perfect fit has r^2 of 1");
    check(!fit_scalability({1.0}, {1.0}).defined, "one point is not a fit");

    check(std::fabs(geometric_mean({1.0, 4.0}) - 2.0) < 1e-12, "geometric mean");

    const auto distribution = bootstrap_distribution(values, 100, 7);
    check(distribution.size() == 100, "bootstrap distribution size");
    check(distribution == bootstrap_distribution(values, 100, 7),
          "bootstrap distribution is deterministic");

    if (failures != 0) {
        std::cerr << failures << " statistics check(s) failed\n";
        return 1;
    }
    std::cout << "statistics tests passed\n";
    return 0;
}
