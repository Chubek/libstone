#pragma once

#include "cephyr/Cephyr.h"

#include <memory>
#include <span>
#include <string_view>

namespace cephyr {

class Diagnostics;

// Request-local. The aggregate SDK and its Lua closures are valid only while
// this object is alive. Populate it before preparing an SDK pipeline.
class LuaExtensions {
  public:
    LuaExtensions();
    ~LuaExtensions();
    LuaExtensions(const LuaExtensions &) = delete;
    LuaExtensions &operator=(const LuaExtensions &) = delete;

    // Native registrations are borrowed and precede Lua registrations.
    // Call before add(); the driver validates the resulting SDK as usual.
    bool set_native(const cephyr_sdk *native, Diagnostics &diagnostics);
    bool add(std::string_view source, std::string_view name,
             std::span<const cephyr_lua_env_entry> environment, Diagnostics &diagnostics);
    [[nodiscard]] const cephyr_sdk *sdk() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cephyr
