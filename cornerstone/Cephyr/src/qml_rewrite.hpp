#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cephyr {

class Diagnostics;

namespace qml {

// A configured plan is immutable during rewrite; const calls may run concurrently.
// Native rules match one pure i64 SSA root with flat operands. A replacement is
// another supported root using the original operands, a copy, or an i64 const.
// Literal patterns use earlier definitions in the same block, including pure
// integer arithmetic and width normalization. Structure checking is not a
// proof of the semantic equivalence asserted by the supplied rule.
class Pipeline {
  public:
    // Append a CEPHYR-QML-1 plan, in rule priority order. A successful append
    // requires reconfiguration; a failed append leaves the old plan intact.
    bool add_compiled(std::string_view text, Diagnostics &diag, std::string_view source);

    // An empty list uses the compiled P directives, or declaration order when
    // none exist. Fuel counts changing rewrites across one entire MIR module.
    bool configure(const std::vector<std::string> &phases, unsigned fuel, Diagnostics &diag);

    // Each named phase can also be given an individual cap per invocation.
    bool configure(const std::vector<std::string> &phases, unsigned fuel, Diagnostics &diag,
                   const std::map<std::string, unsigned, std::less<>> &phase_fuel);

    // Operates on Cephyr's line-oriented Archon MachineIR v1 output. On
    // failure, including a rejection or exhausted fuel, mir is unchanged.
    // Callers should still run the native verifier on changed output.
    bool rewrite(std::string &mir, Diagnostics &diag, std::string_view input) const;

    // Fast path only for the native text produced immediately by Cephyr's
    // lower(), before any external rewrite. Skips structural verification of
    // the original; the caller must verify any changed output. If supplied,
    // changed is true only when this call successfully changes the MIR bytes.
    bool rewrite_canonical(std::string &mir, Diagnostics &diag, std::string_view input,
                           bool *changed = nullptr) const;

  private:
    struct State;
    std::shared_ptr<const State> state_;

    bool rewrite_impl(std::string &mir, Diagnostics &diag, std::string_view input,
                      bool verify_input, bool *changed) const;
};

} // namespace qml
} // namespace cephyr
