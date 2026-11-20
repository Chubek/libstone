#ifndef CEPHYR_SDK_INTERNAL_HPP
#define CEPHYR_SDK_INTERNAL_HPP

#include "cephyr/CephyrSDK.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cephyr {

class Diagnostics;
struct Target;

// A request-local pipeline. Call validate() before running any stage, even
// when the selected output mode wouldn't invoke callbacks.
class SdkPipeline {
  public:
    explicit SdkPipeline(const cephyr_sdk *sdk) : sdk_(sdk) {}

    bool validate(Diagnostics &diagnostics);
    bool desugar(std::string &source, const Target &target,
                 std::string_view input_name, Diagnostics &diagnostics);
    bool analyze(std::string_view mir, const Target &target,
                 std::string_view input_name, Diagnostics &diagnostics);
    bool rewrite(std::string &mir, const Target &target,
                 std::string_view input_name, Diagnostics &diagnostics,
                 bool *changed = nullptr);
    [[nodiscard]] bool has_backend(const Target &target) const;
    std::optional<std::string> emit_assembly(std::string_view mir,
                                             const Target &target,
                                             std::string_view input_name,
                                             bool position_independent,
                                             Diagnostics &diagnostics);

  private:
    struct Hook {
        uint32_t stage;
        std::string name;
        std::string target_triple;
        cephyr_sdk_callback callback;
        void *user;
    };

    bool ready(Diagnostics &diagnostics) const;
    const Hook *find_backend(const Target &target) const;
    bool transform(uint32_t stage, std::string &text, const Target &target,
                   std::string_view input_name, Diagnostics &diagnostics,
                   bool *changed);
    bool invoke(const Hook &hook, std::string_view text, const Target &target,
                std::string_view input_name, uint32_t flags,
                Diagnostics &diagnostics,
                std::optional<std::string> &replacement);

    const cephyr_sdk *sdk_;
    bool valid_ = false;
    std::vector<Hook> hooks_;
    std::size_t diagnostic_count_ = 0;
};

} // namespace cephyr

#endif
