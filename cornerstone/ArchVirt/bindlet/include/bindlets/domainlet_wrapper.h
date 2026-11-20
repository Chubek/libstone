#include <dompybind/dompybind.hpp>
#pragma once
/**
 * domainlet_wrapper.h — C++17 dompybind wrapper for Python domainlets
 * ===================================================================
 *
 * Owns the lifecycle of a single Python domainlet instance and provides
 * a type-safe C++ API for the phases: probe → configure → execute.
 *
 * Thread safety: this class is *not* thread-safe by itself. The wrapper
 * acquires the Python GIL internally for every interaction with Python, so
 * callers may invoke public methods from any thread as long as no two
 * threads share the same DomainletWrapper instance concurrently.
 */

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace bindlets {

// ═══════════════════════════════════════════════════════════════════
//  Error codes (mirrors Python BindletErrorCode)
// ═══════════════════════════════════════════════════════════════════

enum class ErrorCode : int {
    OK = 0,
    NOT_FOUND = 1,
    CONFIG_INVALID = 2,
    VERSION_MISMATCH = 3,
    EXECUTION_FAILED = 4,
    INPUT_MALFORMED = 5,
    TIMEOUT = 6,
    INTERNAL = 99,
};

struct BindletError {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::string source;
    std::string detail;
};

// ═══════════════════════════════════════════════════════════════════
//  Data types
// ═══════════════════════════════════════════════════════════════════

enum class Severity : int {
    STYLE = 0,
    INFORMATION = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4,
};

enum class Confidence : int {
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2,
};

enum class InputKind : int {
    AST_JSON = 0,
    TOKEN_STREAM = 1,
    SOURCE_TEXT = 2,
    CFG = 3,
};

struct SourceSpan {
    std::string file;
    int line_start = 0;
    int line_end = 0;
    int col_start = 0;
    int col_end = 0;
};

struct FixSuggestion {
    std::string description;
    std::string replacement;
    std::optional<SourceSpan> span;
};

struct AnalysisIssue {
    std::string rule_id;
    std::string message;
    Severity severity = Severity::WARNING;
    Confidence confidence = Confidence::MEDIUM;
    SourceSpan location;
    std::vector<int> cwe_ids;
    std::optional<FixSuggestion> fix;
    std::unordered_map<std::string, std::string> extra;
};

struct DomainletCapability {
    std::string name;
    std::string description;
    InputKind input_kind = InputKind::AST_JSON;
    std::vector<std::string> output_kinds;
    std::vector<std::string> tags;
};

struct DomainletInfo {
    std::string name;
    std::string version;
    int api_version = 1;
    std::string description;
    std::string author;
    std::vector<DomainletCapability> capabilities;
};

struct AnalysisContext {
    InputKind kind = InputKind::AST_JSON;
    std::string data;            // payload (JSON, source text, etc.)
    std::string source_file;
    std::string language = "c";
    std::unordered_map<std::string, std::string> defines;
    std::vector<std::string> include_paths;
    std::unordered_map<std::string, std::string> options;
};

// ═══════════════════════════════════════════════════════════════════
//  Result types
// ═══════════════════════════════════════════════════════════════════

template <typename T>
struct BindletResult {
    T value;
    BindletError error;
    bool ok() const { return error.code == ErrorCode::OK; }
};

template <>
struct BindletResult<void> {
    BindletError error;
    bool ok() const { return error.code == ErrorCode::OK; }
};

// ═══════════════════════════════════════════════════════════════════
//  DomainletWrapper
// ═══════════════════════════════════════════════════════════════════

class DomainletWrapper {
public:
    /**
     * Construct a wrapper by importing *module_name* and instantiating
     * *class_name* from the Python domainlet package.
     *
     * The Python interpreter must already be initialized (call
     * ``InitializePythonRuntime`` first).
     *
     * \param module_name   dotted Python module (e.g. "domainlet.bindlet_base")
     * \param class_name    class within that module
     *
     * \throws std::runtime_error if the module/class cannot be loaded.
     */
    DomainletWrapper(std::string_view module_name,
                     std::string_view class_name);

    ~DomainletWrapper();

    // Non-copyable, movable
    DomainletWrapper(const DomainletWrapper&) = delete;
    DomainletWrapper& operator=(const DomainletWrapper&) = delete;
    DomainletWrapper(DomainletWrapper&&) noexcept;
    DomainletWrapper& operator=(DomainletWrapper&&) noexcept;

    // ── lifecycle ─────────────────────────────────────────────────

    /** Lightweight metadata query — does not load heavy resources. */
    BindletResult<DomainletInfo> Probe();

    /** Apply string→string configuration. */
    BindletResult<void> Configure(
        const std::unordered_map<std::string, std::string>& config);

    /** Run analysis and return issues. */
    BindletResult<std::vector<AnalysisIssue>> Execute(
        const AnalysisContext& context);

    /** Release Python-side resources. */
    BindletResult<void> Teardown();

    // ── introspection ─────────────────────────────────────────────

    /** True if the underlying Python instance is valid. */
    explicit operator bool() const { return valid_; }

    /** The fully-qualified Python class name. */
    const std::string& class_name() const { return class_name_; }

private:
    std::string module_name_;
    std::string class_name_;
    dompybind::object instance_;   // Python domainlet instance
    bool valid_ = false;
    bool registered_with_runtime_ = false;

    /** Convert a Python error into a BindletError. */
    static BindletError _translate_py_error(
        const dompybind::python_error& error);

    /** Populate a BindletError from a structured Python BindletError. */
    static BindletError _parse_bindlet_error(dompybind::handle exc);
};

// ═══════════════════════════════════════════════════════════════════
//  Global runtime management
// ═══════════════════════════════════════════════════════════════════

/**
 * Initialize the embedded Python interpreter and add the project's
 * ``domainlet`` package to ``sys.path``.
 *
 * Must be called once before any ``DomainletWrapper`` is constructed.
 * Safe to call multiple times (idempotent).
 */
BindletResult<void> InitializePythonRuntime(std::string_view domainlet_path);

/** Shut down the embedded Python interpreter. */
void ShutdownPythonRuntime();

}  // namespace bindlets
