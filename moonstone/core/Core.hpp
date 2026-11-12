#pragma once
#include "Export.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonstone {
inline namespace v1 {
inline constexpr std::uint32_t api_version = MOONSTONE_API_VERSION;
struct SourceSpan {
    std::string file;
    std::uint32_t line = 1, column = 1, end_line = 1, end_column = 1;
};
enum class Severity { Note, Warning, Error, Fatal };
inline const char *name(Severity s) {
    switch (s) {
    case Severity::Note:
        return "note";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    case Severity::Fatal:
        return "fatal";
    }
    return "error";
}
struct Fix {
    SourceSpan span;
    std::string replacement, description;
    bool validated = false;
};
struct Diagnostic {
    std::string id, message;
    Severity severity = Severity::Error;
    SourceSpan primary;
    std::vector<SourceSpan> related;
    std::vector<std::string> notes;
    std::vector<Fix> fixes;
};
using Diagnostics = std::vector<Diagnostic>;
enum class Status { Proven, Refuted, Unknown, BudgetExhausted, InvalidInput };
struct ProofResult {
    Status status = Status::Unknown;
    std::string explanation;
    std::vector<std::string> assumptions;
    std::size_t work = 0;
    bool is_proven() const { return status == Status::Proven; }
};
MOONSTONE_API std::string json_escape(std::string_view s);
MOONSTONE_API std::string format(const Diagnostic &d);
// Source edits are applied only when byte offsets have been independently checked by the host.
struct SourceEdit {
    std::size_t begin = 0, end = 0;
    std::string replacement;
};
MOONSTONE_API std::optional<std::string> apply_edits(const std::string &source,
                                                     std::vector<SourceEdit> edits);
} // namespace v1
} // namespace moonstone
