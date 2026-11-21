#ifndef CEPHYR_BACKEND_HPP
#define CEPHYR_BACKEND_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cephyr {

class Diagnostics;
struct Config;
struct Target;
namespace factir { struct Result; }

class Backend {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  Backend(const Config &, const Target &,
          const std::filesystem::path &workspace, Diagnostics &);
  ~Backend();

  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;

  bool prepare();
  std::optional<std::string> compile(std::string_view mir,
                                      const std::string &input_name,
                                      bool position_independent = false,
                                      const factir::Result *facts = nullptr);
};

} // namespace cephyr

#endif
