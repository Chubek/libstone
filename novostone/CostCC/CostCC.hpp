#pragma once
#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <array>

namespace novostone::cost {
using Features = std::array<double, 4>; // pure operations, loads, stores, control transfers
struct Context {
  std::string target;
  std::string isa;
  std::string cache;
  std::string optimization;
  std::uint64_t profile_epoch = 0;
  bool operator==(const Context&) const = default;
};
struct Sample { Context context; std::string workload; Features features; double latency; };
struct Prediction { double latency; double confidence; };
Features features(const ccdb::Snapshot& snapshot);
double heuristic(const Features& features);
class Model {
public:
  static Model train(std::string version, const std::vector<Sample>& training,
                     const std::vector<Sample>& calibration, double ridge = 1e-6);
  std::optional<Prediction> predict(const Context& context, const Features& features) const;
  const std::string& version() const { return version_; }
  std::string serialize() const;
  static Model parse(const std::string& text);
private:
  std::string version_;
  Context context_;
  Features minimum_{}, maximum_{};
  std::array<double, 5> weights_{};
  double confidence_ = 0;
  double estimate(const Features& features) const;
};
struct Ranked {
  Features features;
  double baseline_cost;
  std::optional<Prediction> prediction;
};
struct Decision {
  ccdb::Revision revision;
  Context context;
  std::string model_version;
  std::vector<Ranked> candidates;
  std::size_t selected;
  bool used_model;
  std::string reason;
  std::optional<double> observed_latency;
  bool drift = false;
  std::string explain() const;
};
class Ranker {
public:
  explicit Ranker(std::optional<Model> model = {}, double confidence_threshold = 0.8);
  Decision choose(const std::vector<query::Candidate>& candidates, const Context& context);
  // Feedback is logged, never used to change legality or silently mutate a model.
  void observe(std::size_t decision, double latency, double drift_threshold = 0.25);
  bool quarantined() const { return quarantined_; }
  void replace_model(Model model);
  const std::vector<Decision>& log() const { return log_; }
private:
  std::optional<Model> model_;
  double threshold_;
  std::vector<Decision> log_;
  bool quarantined_ = false;
};
} // namespace novostone::cost
