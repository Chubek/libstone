#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace cephyr {

// A zero coordinate is unknown. A column is meaningful only when line is nonzero.
struct Location {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
};

// Stage-local diagnostic collector. Instances are not internally synchronized.
class Diagnostics {
  public:
    explicit Diagnostics(bool warnings_as_errors = false);

    void error(std::string_view code, std::string_view message, Location where = {});
    void warning(std::string_view code, std::string_view message, Location where = {});
    void note(std::string_view code, std::string_view message, Location where = {});

    // Includes warnings when warnings_as_errors() is enabled.
    [[nodiscard]] std::size_t error_count() const noexcept;

    // Promotion applies to all retained warnings, including warnings emitted earlier.
    void set_warnings_as_errors(bool enabled) noexcept;
    [[nodiscard]] bool warnings_as_errors() const noexcept;

    // sarif=false emits one compiler-style diagnostic per line. sarif=true emits
    // one complete SARIF 2.1.0 document.
    void render(std::ostream &out, bool sarif = false) const;

  private:
    enum class Severity { Note, Warning, Error };

    struct Entry {
        std::string code;
        std::string message;
        Location where;
        Severity severity = Severity::Error;
    };

    void emit(Severity severity, std::string_view code, std::string_view message,
              Location where);

    std::vector<Entry> entries_;
    bool warnings_as_errors_ = false;
};

} // namespace cephyr
