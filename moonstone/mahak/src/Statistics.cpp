#include "mahak/Statistics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

std::vector<double> valid_values(const std::vector<Observation> &observations) {
    std::vector<double> values;
    values.reserve(observations.size());
    for (const auto &observation : observations) {
        if (!observation.valid)
            continue;
        if (!std::isfinite(observation.value))
            continue;
        values.push_back(observation.value);
    }
    return values;
}

double median_of_sorted(const std::vector<double> &sorted) {
    if (sorted.empty())
        return 0.0;
    return quantile(sorted, 0.5);
}

double mean_of(const std::vector<double> &values) {
    if (values.empty())
        return 0.0;
    const double total =
        std::accumulate(values.begin(), values.end(), 0.0, [](double sum, double value) {
            return sum + value;
        });
    return total / static_cast<double>(values.size());
}

// Deterministic bootstrap of a statistic over resampled values.
std::vector<double> resample_statistics(const std::vector<double> &values, std::uint32_t resamples,
                                        std::uint64_t seed,
                                        const std::function<double(const std::vector<double> &)> &statistic) {
    std::vector<double> results;
    if (values.empty())
        return results;
    const std::uint32_t count = std::max<std::uint32_t>(1, resamples);
    results.reserve(count);
    Rng rng(seed);
    std::vector<double> sample(values.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        for (auto &slot : sample)
            slot = values[rng.below(values.size())];
        std::sort(sample.begin(), sample.end());
        results.push_back(statistic(sample));
    }
    return results;
}

Error statistics_error(std::string message, ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

} // namespace

double quantile(const std::vector<double> &sorted, double probability) {
    if (sorted.empty())
        return 0.0;
    if (!(probability > 0.0))
        return sorted.front();
    if (probability >= 1.0)
        return sorted.back();
    const double position = probability * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper)
        return sorted[lower];
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

Estimates estimate(const std::vector<Observation> &observations) {
    Estimates result;
    result.total = observations.size();
    std::vector<double> values = valid_values(observations);
    result.valid = values.size();
    if (values.empty())
        return result;
    std::sort(values.begin(), values.end());
    result.minimum = values.front();
    result.maximum = values.back();
    result.mean = mean_of(values);
    result.median = median_of_sorted(values);
    if (values.size() >= 2) {
        double squared = 0.0;
        for (double value : values) {
            const double delta = value - result.mean;
            squared += delta * delta;
        }
        result.variance = squared / static_cast<double>(values.size() - 1);
    }
    result.standard_deviation = std::sqrt(result.variance);
    // Median absolute deviation must be computed in value space, not on the
    // sorted sample, so it is recomputed from the unsorted valid values.
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (double value : values)
        deviations.push_back(std::fabs(value - result.median));
    std::sort(deviations.begin(), deviations.end());
    result.mad = median_of_sorted(deviations);
    result.q1 = quantile(values, 0.25);
    result.q3 = quantile(values, 0.75);
    result.p90 = quantile(values, 0.90);
    result.p95 = quantile(values, 0.95);
    result.p99 = quantile(values, 0.99);
    if (result.mean != 0.0)
        result.relative_standard_deviation = result.standard_deviation / std::fabs(result.mean);
    return result;
}

ConfidenceInterval bootstrap_confidence_interval(const std::vector<double> &values,
                                                 const BootstrapOptions &options) {
    ConfidenceInterval interval;
    interval.confidence = options.confidence;
    std::vector<double> valid;
    valid.reserve(values.size());
    for (double value : values) {
        if (std::isfinite(value))
            valid.push_back(value);
    }
    if (valid.size() < 2) {
        interval.defined = false;
        interval.method = "unavailable";
        return interval;
    }
    std::uint32_t resamples =
        std::clamp<std::uint32_t>(options.resamples, 1, std::max<std::uint32_t>(1, options.max_resamples));
    auto distribution = resample_statistics(valid, resamples, options.seed,
                                            [](const std::vector<double> &sample) {
                                                return mean_of(sample);
                                            });
    std::sort(distribution.begin(), distribution.end());
    const double confidence = std::clamp(options.confidence, 0.0, 1.0);
    const double alpha = (1.0 - confidence) / 2.0;
    interval.lower = quantile(distribution, alpha);
    interval.upper = quantile(distribution, 1.0 - alpha);
    interval.defined = true;
    interval.method = "bootstrap-percentile";
    return interval;
}

OutlierReport detect_outliers(const std::vector<Observation> &observations,
                              const OutlierOptions &options) {
    OutlierReport report;
    report.threshold = options.mad_threshold;
    report.method = "modified-z-mad";
    if (options.method == OutlierOptions::Method::None) {
        report.method = "none";
        report.retained = observations;
        return report;
    }
    // Modified Z-scores need a stable median and MAD; below four valid samples
    // the estimate is too fragile to justify excluding anything.
    std::vector<double> values = valid_values(observations);
    if (values.size() < 4) {
        report.method = "none";
        report.retained = observations;
        return report;
    }
    std::sort(values.begin(), values.end());
    const double median = median_of_sorted(values);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (double value : values)
        deviations.push_back(std::fabs(value - median));
    std::sort(deviations.begin(), deviations.end());
    const double mad = median_of_sorted(deviations);
    // When at least half the samples are identical the MAD collapses to zero;
    // fall back to the mean absolute deviation (without the 0.6745 normalizer,
    // which only applies to the median-based scale) so a large minority outlier
    // is still detected instead of silently retained.
    const double scale = mad > 0.0 ? 0.6745 / mad
                                   : (mean_of(deviations) > 0.0 ? 1.0 / mean_of(deviations)
                                                                : 0.0);
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const Observation &observation = observations[index];
        bool exclude = false;
        if (observation.valid && std::isfinite(observation.value) && scale > 0.0) {
            const double score = std::fabs(observation.value - median) * scale;
            exclude = score > options.mad_threshold;
        }
        if (exclude) {
            report.excluded_indices.push_back(index);
            report.excluded.push_back(observation);
        } else {
            report.retained.push_back(observation);
        }
    }
    return report;
}

Result<Comparison> compare_observations(const std::vector<Observation> &baseline,
                                        const std::vector<Observation> &candidate,
                                        const ComparisonOptions &options) {
    Comparison comparison;
    ComparisonResult &result = comparison.result;
    const bool baseline_named = !baseline.empty();
    const bool candidate_named = !candidate.empty();
    if (baseline_named)
        result.metric = baseline.front().metric;
    if (candidate_named && !candidate.front().metric.empty()) {
        if (!result.metric.empty() && baseline.front().metric != candidate.front().metric)
            return statistics_error("metric identity mismatch: '" + baseline.front().metric +
                                    "' vs '" + candidate.front().metric + "'");
        result.metric = candidate.front().metric;
    }
    if (baseline_named)
        result.unit = baseline.front().unit;
    if (candidate_named) {
        if (!result.unit.empty() && baseline.front().unit != candidate.front().unit)
            return statistics_error("unit mismatch: '" + baseline.front().unit + "' vs '" +
                                    candidate.front().unit + "'");
        result.unit = candidate.front().unit;
    }
    result.direction = baseline_named ? baseline.front().direction
                                      : (candidate_named ? candidate.front().direction
                                                         : MetricDirection::LowerIsBetter);
    if (baseline_named && candidate_named && baseline.front().direction != candidate.front().direction)
        return statistics_error("metric direction mismatch");

    OutlierReport baseline_outliers = detect_outliers(baseline, options.outliers);
    OutlierReport candidate_outliers = detect_outliers(candidate, options.outliers);
    comparison.baseline_retained = baseline_outliers.retained;
    comparison.candidate_retained = candidate_outliers.retained;
    comparison.baseline_excluded = baseline_outliers.excluded;
    comparison.candidate_excluded = candidate_outliers.excluded;

    const Estimates baseline_estimates = estimate(baseline_outliers.retained);
    const Estimates candidate_estimates = estimate(candidate_outliers.retained);
    result.baseline_mean = baseline_estimates.mean;
    result.candidate_mean = candidate_estimates.mean;
    result.baseline_median = baseline_estimates.median;
    result.candidate_median = candidate_estimates.median;

    const std::vector<double> baseline_values = valid_values(baseline_outliers.retained);
    const std::vector<double> candidate_values = valid_values(candidate_outliers.retained);
    result.absolute_change = result.candidate_mean - result.baseline_mean;
    if (result.baseline_mean == 0.0) {
        result.ratio = result.candidate_mean == 0.0 ? 1.0
                                                    : std::numeric_limits<double>::infinity();
        result.percent_change = result.absolute_change == 0.0
                                    ? 0.0
                                    : std::numeric_limits<double>::infinity();
    } else {
        result.percent_change = 100.0 * result.absolute_change / result.baseline_mean;
        result.ratio = result.candidate_mean / result.baseline_mean;
    }
    if (result.direction == MetricDirection::HigherIsBetter)
        result.speedup = result.ratio;
    else
        result.speedup = result.ratio == 0.0 ? std::numeric_limits<double>::infinity()
                                             : 1.0 / result.ratio;
    result.geometric_mean_ratio = result.ratio;

    if (baseline_estimates.valid < options.criteria.minimum_samples ||
        candidate_estimates.valid < options.criteria.minimum_samples) {
        result.verdict = "insufficient-data";
        result.message = "need at least " + std::to_string(options.criteria.minimum_samples) +
                         " valid samples per side";
        return comparison;
    }
    if (result.baseline_mean == 0.0) {
        // A zero baseline makes a relative comparison undefined; the absolute
        // change is still reported, but no regression verdict is claimed.
        result.verdict = "incompatible";
        result.message = "baseline mean is zero; relative change is undefined";
        return comparison;
    }

    BootstrapOptions bootstrap;
    bootstrap.seed = options.seed;
    bootstrap.resamples = options.bootstrap_resamples;
    bootstrap.confidence = options.confidence;

    // A paired comparison requires equal series and pairs by observation
    // sequence; its statistic is the mean of per-pair differences. An unpaired
    // comparison bootstraps the difference of means independently.
    if (options.paired && baseline_values.size() == candidate_values.size() &&
        !baseline_values.empty()) {
        std::vector<double> differences;
        differences.reserve(baseline_values.size());
        for (std::size_t index = 0; index < baseline_values.size(); ++index)
            differences.push_back(baseline_values[index] - candidate_values[index]);
        result.interval = bootstrap_confidence_interval(differences, bootstrap);
        result.interval.method = "bootstrap-percentile-paired";
    } else {
        Rng rng(options.seed);
        auto difference_distribution = [&] {
            std::vector<double> distribution;
            distribution.reserve(options.bootstrap_resamples == 0 ? 1 : options.bootstrap_resamples);
            for (std::uint32_t index = 0; index < options.bootstrap_resamples; ++index) {
                double baseline_sum = 0.0;
                for (std::size_t draw = 0; draw < baseline_values.size(); ++draw)
                    baseline_sum += baseline_values[rng.below(baseline_values.size())];
                double candidate_sum = 0.0;
                for (std::size_t draw = 0; draw < candidate_values.size(); ++draw)
                    candidate_sum += candidate_values[rng.below(candidate_values.size())];
                distribution.push_back(candidate_sum / static_cast<double>(candidate_values.size()) -
                                       baseline_sum / static_cast<double>(baseline_values.size()));
            }
            return distribution;
        };
        if (!baseline_values.empty() && !candidate_values.empty()) {
            std::vector<double> distribution = difference_distribution();
            std::sort(distribution.begin(), distribution.end());
            if (distribution.size() >= 2) {
                const double alpha = (1.0 - std::clamp(options.confidence, 0.0, 1.0)) / 2.0;
                result.interval.lower = quantile(distribution, alpha);
                result.interval.upper = quantile(distribution, 1.0 - alpha);
                result.interval.confidence = options.confidence;
                result.interval.method = "bootstrap-percentile-unpaired";
                result.interval.defined = true;
            }
        }
    }

    result.statistically_significant =
        result.interval.defined && (result.interval.lower > 0.0 || result.interval.upper < 0.0);
    const double relative = std::fabs(result.percent_change) / 100.0;
    result.practical_change = relative >= options.criteria.practical_threshold &&
                              relative >= options.criteria.minimum_relative_change;
    const bool evidence = options.criteria.require_statistical_significance
                              ? result.statistically_significant
                              : result.practical_change;
    const bool worse = result.direction == MetricDirection::LowerIsBetter
                           ? result.candidate_mean > result.baseline_mean
                           : result.candidate_mean < result.baseline_mean;
    const bool better = result.direction == MetricDirection::LowerIsBetter
                            ? result.candidate_mean < result.baseline_mean
                            : result.candidate_mean > result.baseline_mean;
    if (evidence && result.practical_change && worse) {
        result.verdict = "regression";
        result.regression = true;
    } else if (evidence && result.practical_change && better) {
        result.verdict = "improvement";
        result.improvement = true;
    } else {
        result.verdict = "unchanged";
    }
    return comparison;
}

double geometric_mean(const std::vector<double> &values) {
    double log_sum = 0.0;
    std::size_t count = 0;
    for (double value : values) {
        if (!(value > 0.0) || !std::isfinite(value))
            continue;
        log_sum += std::log(value);
        ++count;
    }
    if (count == 0)
        return 0.0;
    return std::exp(log_sum / static_cast<double>(count));
}

ScalabilityFit fit_scalability(const std::vector<double> &x, const std::vector<double> &y) {
    ScalabilityFit fit;
    fit.model = "log-log";
    const std::size_t count = std::min(x.size(), y.size());
    std::vector<double> log_x;
    std::vector<double> log_y;
    log_x.reserve(count);
    log_y.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (!(x[index] > 0.0) || !(y[index] > 0.0))
            continue;
        log_x.push_back(std::log(x[index]));
        log_y.push_back(std::log(y[index]));
    }
    fit.points = log_x.size();
    if (log_x.size() < 2)
        return fit;
    const double mean_x = mean_of(log_x);
    const double mean_y = mean_of(log_y);
    double covariance = 0.0;
    double variance_x = 0.0;
    for (std::size_t index = 0; index < log_x.size(); ++index) {
        const double dx = log_x[index] - mean_x;
        covariance += dx * (log_y[index] - mean_y);
        variance_x += dx * dx;
    }
    if (!(variance_x > 0.0))
        return fit;
    fit.exponent = covariance / variance_x;
    fit.intercept = std::exp(mean_y - fit.exponent * mean_x);
    double total = 0.0;
    double residual = 0.0;
    for (std::size_t index = 0; index < log_x.size(); ++index) {
        const double predicted = std::log(fit.intercept) + fit.exponent * log_x[index];
        total += (log_y[index] - mean_y) * (log_y[index] - mean_y);
        residual += (log_y[index] - predicted) * (log_y[index] - predicted);
    }
    fit.r_squared = total > 0.0 ? 1.0 - residual / total : 0.0;
    fit.defined = true;
    return fit;
}

std::vector<ResidualPoint> scalability_residuals(const std::vector<double> &x,
                                                 const std::vector<double> &y,
                                                 const ScalabilityFit &fit) {
    std::vector<ResidualPoint> points;
    const std::size_t count = std::min(x.size(), y.size());
    points.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ResidualPoint point;
        point.x = x[index];
        point.observed = y[index];
        point.predicted = fit.defined ? fit.intercept * std::pow(x[index], fit.exponent) : 0.0;
        point.residual = point.observed - point.predicted;
        points.push_back(point);
    }
    return points;
}

std::vector<double> bootstrap_distribution(const std::vector<double> &values,
                                           std::uint32_t resamples, std::uint64_t seed) {
    std::vector<double> valid;
    valid.reserve(values.size());
    for (double value : values) {
        if (std::isfinite(value))
            valid.push_back(value);
    }
    return resample_statistics(valid, resamples, seed,
                               [](const std::vector<double> &sample) { return mean_of(sample); });
}

} // namespace v1
} // namespace moonstone::mahak
