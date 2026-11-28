#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Model.hpp"
#include "Rng.hpp"

namespace moonstone::mahak {
inline namespace v1 {

// Linear-interpolated quantile (Hyndman-Fan type 7) over an ascending vector.
// An empty input is invalid (returns 0). NaN inputs are caller errors.
double quantile(const std::vector<double> &sorted, double probability);

// Computes robust and classical estimators from the valid observations only.
// Invalid observations are counted in `total` but never enter the estimators.
Estimates estimate(const std::vector<Observation> &observations);

// Percentile bootstrap. Returns defined=false with method="unavailable" when
// there are fewer than two valid observations. Resampling is deterministic for
// a fixed seed and uses only valid observations.
ConfidenceInterval bootstrap_confidence_interval(const std::vector<double> &values,
                                                 const BootstrapOptions &options);

// Modified Z-score (median absolute deviation) outlier detection. Excluded
// observations are retained in the report and can remain in CSV/JSON exports.
OutlierReport detect_outliers(const std::vector<Observation> &observations,
                              const OutlierOptions &options);

struct Comparison {
    ComparisonResult result;
    std::vector<Observation> baseline_retained;
    std::vector<Observation> candidate_retained;
    std::vector<Observation> baseline_excluded;
    std::vector<Observation> candidate_excluded;
};

// Compares two observation series. Incompatible metric identity (name, unit or
// direction) is an InvalidInput error. Insufficient samples yield a valid
// ComparisonResult with verdict "insufficient-data" and no regression decision.
// Paired comparisons pair by observation sequence; unpaired comparisons use an
// independent two-sample bootstrap.
Result<Comparison> compare_observations(const std::vector<Observation> &baseline,
                                        const std::vector<Observation> &candidate,
                                        const ComparisonOptions &options);

// Geometric mean over strictly positive values. Returns 0 for an empty input.
double geometric_mean(const std::vector<double> &values);

// Log-log least squares fit y = a * x^b. Fewer than two positive points or a
// degenerate x range yields defined=false; the caller must report it as
// unsupported rather than as an exponent of zero.
ScalabilityFit fit_scalability(const std::vector<double> &x, const std::vector<double> &y);

struct ResidualPoint {
    double x = 0.0;
    double observed = 0.0;
    double predicted = 0.0;
    double residual = 0.0;
};

std::vector<ResidualPoint> scalability_residuals(const std::vector<double> &x,
                                                 const std::vector<double> &y,
                                                 const ScalabilityFit &fit);

// Deterministic bootstrap distribution of a statistic (used by reports/tests).
std::vector<double> bootstrap_distribution(const std::vector<double> &values,
                                           std::uint32_t resamples, std::uint64_t seed);

} // namespace v1
} // namespace moonstone::mahak
