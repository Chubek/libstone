#pragma once

#include <cephyr/CephyrPlugin.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace cephyr {

class Diagnostics;

// Holds callback code and all borrowed registration storage live until
// after compilation. Populate before constructing the active SDK pipeline.
class NativePlugins {
  public:
    NativePlugins() = default;
    NativePlugins(const NativePlugins &) = delete;
    NativePlugins &operator=(const NativePlugins &) = delete;

    // The driver must check explicit opt-in before calling this.
    bool load(const std::vector<std::filesystem::path> &paths,
              const cephyr_sdk *request_sdk, Diagnostics &diagnostics);
    [[nodiscard]] const cephyr_sdk *sdk() const { return &sdk_; }

  private:
    struct CloseModule {
        void operator()(void *handle) const;
    };
    std::vector<std::unique_ptr<void, CloseModule>> handles_;
    std::vector<cephyr_sdk_hook> hooks_;
    std::vector<cephyr_sdk_directive> directives_;
    cephyr_sdk sdk_{sizeof(cephyr_sdk), CEPHYR_SDK_ABI_VERSION, nullptr, 0, nullptr, 0};
};

} // namespace cephyr
