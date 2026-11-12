#include "novostone/CostCC/CostCC.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <charconv>
#include <sstream>

namespace novostone::cost {
namespace {
void validate(const Features& f) {
  for (auto value : f) if (!std::isfinite(value) || value < 0) throw std::invalid_argument("invalid cost feature");
}
void positive(double value, const char* message) {
  if (!std::isfinite(value) || value <= 0) throw std::invalid_argument(message);
}
}
Features features(const ccdb::Snapshot& snapshot) {
  ir::verify(snapshot);
  Features result{};
  for (const auto& [id, block] : ir::load(snapshot).blocks) {
    (void)id;
    for (const auto& i : block.instructions) {
      if (ir::pure(i.op)) ++result[0];
      else if (i.op == ir::Op::Load) ++result[1];
      else if (i.op == ir::Op::Store) ++result[2];
      else ++result[3];
    }
  }
  return result;
}
double heuristic(const Features& f) { validate(f); return f[0] + 4 * f[1] + 4 * f[2] + 2 * f[3]; }
double Model::estimate(const Features& f) const {
  double value = weights_[0];
  for (std::size_t i = 0; i < f.size(); ++i)
    value += weights_[i + 1] * ((f[i] - minimum_[i]) / std::max(1.0, maximum_[i] - minimum_[i]));
  return value;
}
Model Model::train(std::string version, const std::vector<Sample>& training,
                   const std::vector<Sample>& calibration, double ridge) {
  positive(ridge, "ridge must be positive");
  if (version.empty() || training.size() < 5 || calibration.size() < 2) throw std::invalid_argument("model needs version, five training and two calibration samples");
  Model model;
  model.version_ = std::move(version); model.context_ = training.front().context;
  if (model.context_.target.empty()) throw std::invalid_argument("model requires a target");
  model.minimum_ = training.front().features; model.maximum_ = model.minimum_;
  std::set<std::string> workloads;
  auto check = [&](const Sample& sample) {
    validate(sample.features); positive(sample.latency, "invalid latency");
    if (sample.context != model.context_ || sample.workload.empty()) throw std::invalid_argument("sample context/workload mismatch");
  };
  for (const auto& sample : training) {
    check(sample); workloads.insert(sample.workload);
    for (std::size_t i = 0; i < 4; ++i) {
      model.minimum_[i] = std::min(model.minimum_[i], sample.features[i]);
      model.maximum_[i] = std::max(model.maximum_[i], sample.features[i]);
    }
  }
  for (const auto& sample : calibration) {
    check(sample);
    if (workloads.count(sample.workload)) throw std::invalid_argument("calibration workload leaks into training");
    for (std::size_t i = 0; i < 4; ++i)
      if (sample.features[i] < model.minimum_[i] || sample.features[i] > model.maximum_[i])
        throw std::invalid_argument("calibration sample outside training feature range");
  }
  std::array<std::array<double, 6>, 5> normal{};
  for (const auto& sample : training) {
    std::array<double, 5> x{1, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i)
      x[i + 1] = (sample.features[i] - model.minimum_[i]) / std::max(1.0, model.maximum_[i] - model.minimum_[i]);
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) normal[i][j] += x[i] * x[j];
      normal[i][5] += x[i] * sample.latency;
    }
  }
  for (std::size_t i = 0; i < 5; ++i) normal[i][i] += ridge;
  for (std::size_t column = 0; column < 5; ++column) {
    auto pivot = column;
    for (std::size_t row = column + 1; row < 5; ++row)
      if (std::abs(normal[row][column]) > std::abs(normal[pivot][column])) pivot = row;
    std::swap(normal[pivot], normal[column]);
    double divisor = normal[column][column];
    if (!std::isfinite(divisor) || std::abs(divisor) < 1e-15) throw std::invalid_argument("ill-conditioned cost training data");
    for (std::size_t j = column; j < 6; ++j) normal[column][j] /= divisor;
    for (std::size_t row = 0; row < 5; ++row) if (row != column) {
      auto multiplier = normal[row][column];
      for (std::size_t j = column; j < 6; ++j) normal[row][j] -= multiplier * normal[column][j];
    }
  }
  for (std::size_t i = 0; i < 5; ++i) {
    model.weights_[i] = normal[i][5];
    if (!std::isfinite(model.weights_[i])) throw std::invalid_argument("non-finite model weights");
  }
  double relative_error = 0;
  for (const auto& sample : calibration) {
    auto estimate = model.estimate(sample.features);
    if (!std::isfinite(estimate) || estimate <= 0) { relative_error = std::numeric_limits<double>::infinity(); break; }
    auto error = (estimate - sample.latency) / sample.latency;
    relative_error = std::hypot(relative_error, error);
  }
  model.confidence_ = 1 / (1 + relative_error / std::sqrt(static_cast<double>(calibration.size())));
  return model;
}
std::optional<Prediction> Model::predict(const Context& context, const Features& f) const {
  validate(f);
  if (context != context_) return {};
  for (std::size_t i = 0; i < 4; ++i) if (f[i] < minimum_[i] || f[i] > maximum_[i]) return {};
  auto value = estimate(f);
  if (!std::isfinite(value) || value <= 0) return {};
  return Prediction{value, confidence_};
}
std::string Model::serialize() const {
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10)
      << "novostone-cost 1\n" << std::quoted(version_) << '\n'
      << std::quoted(context_.target) << ' ' << std::quoted(context_.isa) << ' ' << std::quoted(context_.cache)
      << ' ' << std::quoted(context_.optimization) << ' ' << context_.profile_epoch << '\n' << confidence_ << '\n';
  for (auto value : minimum_) out << value << ' ';
  out << '\n';
  for (auto value : maximum_) out << value << ' ';
  out << '\n';
  for (auto weight : weights_) out << weight << ' ';
  out << "\nend\n";
  return out.str();
}
Model Model::parse(const std::string& text) {
  if (text.size() > 1024 * 1024) throw std::invalid_argument("cost model byte limit exceeded");
  std::istringstream in(text); in.imbue(std::locale::classic());
  Model model; std::string token, epoch; int version;
  if (!(in >> token >> version) || token != "novostone-cost" || version != 1 ||
      !(in >> std::quoted(model.version_) >> std::quoted(model.context_.target) >> std::quoted(model.context_.isa)
           >> std::quoted(model.context_.cache) >> std::quoted(model.context_.optimization) >> epoch >> model.confidence_) ||
      model.version_.empty() || model.context_.target.empty() || !std::isfinite(model.confidence_) || model.confidence_ < 0 || model.confidence_ > 1)
    throw std::invalid_argument("invalid cost model header");
  auto parsed_epoch = std::from_chars(epoch.data(), epoch.data() + epoch.size(), model.context_.profile_epoch);
  if (parsed_epoch.ec != std::errc{} || parsed_epoch.ptr != epoch.data() + epoch.size()) throw std::invalid_argument("invalid model epoch");
  for (auto& value : model.minimum_) if (!(in >> value)) throw std::invalid_argument("invalid model minimum");
  for (auto& value : model.maximum_) if (!(in >> value)) throw std::invalid_argument("invalid model maximum");
  validate(model.minimum_); validate(model.maximum_);
  for (std::size_t i = 0; i < 4; ++i) if (model.minimum_[i] > model.maximum_[i]) throw std::invalid_argument("inverted model feature range");
  for (auto& weight : model.weights_) if (!(in >> weight) || !std::isfinite(weight)) throw std::invalid_argument("invalid model weight");
  if (!(in >> token) || token != "end" || (in >> token)) throw std::invalid_argument("truncated or trailing model data");
  return model;
}
Ranker::Ranker(std::optional<Model> model, double threshold) : model_(std::move(model)), threshold_(threshold) {
  if (!std::isfinite(threshold) || threshold < 0 || threshold > 1) throw std::invalid_argument("invalid confidence threshold");
}
Decision Ranker::choose(const std::vector<query::Candidate>& candidates, const Context& context) {
  if (candidates.empty() || context.target.empty()) throw std::invalid_argument("ranking requires candidates and target");
  Decision decision{candidates.front().base().revision(), context, model_ ? model_->version() : "heuristic",
    {}, 0, model_.has_value() && !quarantined_, {}, {}, false};
  for (const auto& candidate : candidates) {
    if (!candidate.base().same_revision(candidates.front().base())) throw std::invalid_argument("candidates have different semantic roots");
    auto f = features(candidate.result());
    auto prediction = model_ ? model_->predict(context, f) : std::nullopt;
    if (!prediction || prediction->confidence < threshold_) decision.used_model = false;
    decision.candidates.push_back({f, heuristic(f), prediction});
  }
  auto score = [&](std::size_t i) { return decision.used_model ? decision.candidates[i].prediction->latency : decision.candidates[i].baseline_cost; };
  for (std::size_t i = 1; i < candidates.size(); ++i) if (score(i) < score(decision.selected)) decision.selected = i;
  decision.reason = decision.used_model ? "calibrated in-range model" :
    quarantined_ ? "model quarantined after measurement drift; heuristic fallback" : "deterministic heuristic fallback";
  log_.push_back(decision);
  return decision;
}
void Ranker::observe(std::size_t index, double latency, double threshold) {
  positive(latency, "invalid observed latency"); positive(threshold, "invalid drift threshold");
  auto& decision = log_.at(index);
  decision.observed_latency = latency;
  auto prediction = decision.candidates.at(decision.selected).prediction;
  decision.drift = prediction && std::abs(prediction->latency - latency) / latency > threshold;
  if (decision.used_model && decision.drift && model_ && decision.model_version == model_->version()) quarantined_ = true;
}
void Ranker::replace_model(Model model) {
  if (model.version().empty()) throw std::invalid_argument("replacement model needs a version");
  if ((model_ && model_->version() == model.version()) ||
      std::any_of(log_.begin(), log_.end(), [&](const auto& decision) { return decision.model_version == model.version(); }))
    throw std::invalid_argument("replacement requires a fresh model version");
  model_ = std::move(model); quarantined_ = false;
}
std::string Decision::explain() const {
  std::ostringstream out;
  out << "revision " << revision << " target " << std::quoted(context.target) << " isa " << std::quoted(context.isa)
      << " cache " << std::quoted(context.cache) << " context " << std::quoted(context.optimization)
      << " epoch " << context.profile_epoch << " model " << std::quoted(model_version)
      << " selected " << selected << " (" << reason << ")\n";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    out << i << " features";
    for (auto feature : candidates[i].features) out << ' ' << feature;
    out << " heuristic " << candidates[i].baseline_cost;
    if (candidates[i].prediction) out << " predicted " << candidates[i].prediction->latency << " confidence " << candidates[i].prediction->confidence;
    out << '\n';
  }
  if (observed_latency) out << "observed " << *observed_latency << " drift " << drift << '\n';
  return out.str();
}
} // namespace novostone::cost
