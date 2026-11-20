#include <dompybind/dompybind.hpp>
/**
 * domainlet_wrapper.cpp — dompybind bridge implementation
 */

#include "bindlets/domainlet_wrapper.h"

#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <filesystem>


namespace py = dompybind;

namespace bindlets {
namespace {

std::mutex s_runtime_mutex;
bool s_python_initialized = false;
bool s_runtime_owns_interpreter = false;
std::size_t s_live_wrappers = 0;

BindletError internal_error(std::string message, std::string source = {}) {
    return {ErrorCode::INTERNAL, std::move(message), std::move(source), {}};
}

ErrorCode checked_error_code(int value) {
    switch (value) {
        case static_cast<int>(ErrorCode::OK):
            return ErrorCode::OK;
        case static_cast<int>(ErrorCode::NOT_FOUND):
            return ErrorCode::NOT_FOUND;
        case static_cast<int>(ErrorCode::CONFIG_INVALID):
            return ErrorCode::CONFIG_INVALID;
        case static_cast<int>(ErrorCode::VERSION_MISMATCH):
            return ErrorCode::VERSION_MISMATCH;
        case static_cast<int>(ErrorCode::EXECUTION_FAILED):
            return ErrorCode::EXECUTION_FAILED;
        case static_cast<int>(ErrorCode::INPUT_MALFORMED):
            return ErrorCode::INPUT_MALFORMED;
        case static_cast<int>(ErrorCode::TIMEOUT):
            return ErrorCode::TIMEOUT;
        case static_cast<int>(ErrorCode::INTERNAL):
            return ErrorCode::INTERNAL;
        default:
            return ErrorCode::INTERNAL;
    }
}

bool has_field(py::handle object, const char* name) {
    if (py::isinstance<py::dict>(object)) {
        return py::cast<py::dict>(object).contains(name);
    }
    return py::hasattr(object, name);
}

py::object field(py::handle object, const char* name) {
    if (py::isinstance<py::dict>(object)) {
        py::dict dict = py::cast<py::dict>(object);
        if (!dict.contains(name)) {
            throw std::runtime_error(std::string("missing required field '") + name + "'");
        }
        return py::borrow<py::object>(dict[name]);
    }
    if (!py::hasattr(object, name)) {
        throw std::runtime_error(std::string("missing required attribute '") + name + "'");
    }
    return object.attr(name);
}

py::object optional_field(py::handle object, const char* name) {
    return has_field(object, name) ? field(object, name) : py::object();
}

std::string enum_or_string(py::handle value) {
    if (py::hasattr(value, "value")) {
        value = value.attr("value");
    }
    return py::cast<std::string>(value);
}

InputKind input_kind_from_python(py::handle value) {
    const std::string kind = enum_or_string(value);
    if (kind == "ast_json") {
        return InputKind::AST_JSON;
    }
    if (kind == "token_stream") {
        return InputKind::TOKEN_STREAM;
    }
    if (kind == "source_text") {
        return InputKind::SOURCE_TEXT;
    }
    if (kind == "cfg") {
        return InputKind::CFG;
    }
    throw std::runtime_error("unknown input kind '" + kind + "'");
}

template <typename Enum>
Enum checked_enum(py::handle value, int minimum, int maximum,
                  const char* name) {
    const int raw = py::cast<int>(value);
    if (raw < minimum || raw > maximum) {
        throw std::runtime_error(std::string("invalid ") + name + " value");
    }
    return static_cast<Enum>(raw);
}

SourceSpan parse_span(py::handle value) {
    SourceSpan span;
    if (!value.is_valid() || value.is_none()) {
        return span;
    }
    if (has_field(value, "file")) {
        span.file = py::cast<std::string>(field(value, "file"));
    }
    if (has_field(value, "line_start")) {
        span.line_start = py::cast<int>(field(value, "line_start"));
    }
    if (has_field(value, "line_end")) {
        span.line_end = py::cast<int>(field(value, "line_end"));
    }
    if (has_field(value, "col_start")) {
        span.col_start = py::cast<int>(field(value, "col_start"));
    }
    if (has_field(value, "col_end")) {
        span.col_end = py::cast<int>(field(value, "col_end"));
    }
    return span;
}

std::vector<int> parse_cwe_ids(py::handle value) {
    std::vector<int> ids;
    if (!value.is_valid() || value.is_none()) {
        return ids;
    }
    if (py::isinstance<py::str>(value)) {
        std::istringstream stream(py::cast<std::string>(value));
        std::string token;
        while (std::getline(stream, token, ',')) {
            if (!token.empty()) {
                ids.push_back(std::stoi(token));
            }
        }
        return ids;
    }

    py::iterable iterable = py::cast<py::iterable>(value);
    for (py::handle item : iterable) {
        ids.push_back(py::cast<int>(item));
    }
    return ids;
}

std::optional<FixSuggestion> parse_fix(py::handle issue) {
    py::object fix = optional_field(issue, "fix");
    if (fix.is_valid() && !fix.is_none()) {
        FixSuggestion result;
        result.description = py::cast<std::string>(field(fix, "description"));
        if (has_field(fix, "replacement")) {
            result.replacement = py::cast<std::string>(field(fix, "replacement"));
        }
        if (has_field(fix, "span")) {
            py::object span = field(fix, "span");
            if (!span.is_none()) {
                result.span = parse_span(span);
            }
        }
        return result;
    }

    if (!has_field(issue, "fix_description")) {
        return std::nullopt;
    }
    FixSuggestion result;
    result.description = py::cast<std::string>(field(issue, "fix_description"));
    if (has_field(issue, "fix_replacement")) {
        result.replacement = py::cast<std::string>(field(issue, "fix_replacement"));
    }
    return result;
}

AnalysisIssue parse_issue(py::handle object) {
    AnalysisIssue issue;
    issue.rule_id = py::cast<std::string>(field(object, "rule_id"));
    issue.message = py::cast<std::string>(field(object, "message"));

    if (has_field(object, "severity")) {
        issue.severity = checked_enum<Severity>(
            field(object, "severity"), static_cast<int>(Severity::STYLE),
            static_cast<int>(Severity::CRITICAL), "severity");
    }
    if (has_field(object, "confidence")) {
        issue.confidence = checked_enum<Confidence>(
            field(object, "confidence"), static_cast<int>(Confidence::LOW),
            static_cast<int>(Confidence::HIGH), "confidence");
    }

    if (has_field(object, "location")) {
        issue.location = parse_span(field(object, "location"));
    } else {
        issue.location = parse_span(object);
    }
    if (has_field(object, "cwe_ids")) {
        issue.cwe_ids = parse_cwe_ids(field(object, "cwe_ids"));
    }
    issue.fix = parse_fix(object);
    if (has_field(object, "extra")) {
        py::dict extra = py::cast<py::dict>(field(object, "extra"));
        for (auto [key, value] : extra) {
            issue.extra.emplace(
                py::cast<std::string>(key),
                py::cast<std::string>(value));
        }
    }
    return issue;
}

void prepend_sys_path_once(std::string_view domainlet_path) {
    py::module_ sys = py::module_::import_("sys");
    py::list path = py::cast<py::list>(sys.attr("path"));
    const std::string requested(domainlet_path);
    for (py::handle entry : path) {
        if (py::cast<std::string>(entry) == requested) {
            return;
        }
    }
    path.attr("insert")(0, py::str(requested.c_str()));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════
//  Static state for the embedded Python runtime
// ═══════════════════════════════════════════════════════════════════

BindletResult<void> InitializePythonRuntime(std::string_view domainlet_path) {
    BindletResult<void> result;
    std::lock_guard<std::mutex> lock(s_runtime_mutex);

    if (domainlet_path.empty()) {
        result.error = internal_error(
            "domainlet_path must name the directory containing the domainlet package",
            "InitializePythonRuntime");
        return result;
    }

    if (!s_python_initialized) {
        if (!Py_IsInitialized()) {
            Py_Initialize();
            s_runtime_owns_interpreter = true;
            PyEval_SaveThread();
        }
        s_python_initialized = true;
    }

    try {
        py::gil_scoped_acquire gil;
        prepend_sys_path_once(domainlet_path);
        {
            std::error_code ec;
            const auto domsleigh_src =
                std::filesystem::path(domainlet_path) / "domlibs" /
                "domsleigh" / "src";
            if (std::filesystem::is_directory(domsleigh_src, ec)) {
                prepend_sys_path_once(domsleigh_src.string());
            }
        }

        // Test fixtures live under `tests/bindlet-test-suite/` in the monorepo.
        // When the runtime is booted with the repo root (as the test suite does),
        // make those fixtures importable without requiring packaging.
        {
            std::error_code ec;
            const std::filesystem::path root(domainlet_path);
            const auto fixture_dir = root / "tests" / "bindlet-test-suite";
            if (std::filesystem::exists(fixture_dir, ec) && std::filesystem::is_directory(fixture_dir, ec)) {
                prepend_sys_path_once(fixture_dir.string());
            }
        }
    } catch (const py::python_error& error) {
        result.error = internal_error(error.what(), "InitializePythonRuntime");
    } catch (const std::exception& error) {
        result.error = internal_error(error.what(), "InitializePythonRuntime");
    } catch (...) {
        result.error = internal_error(
            "unknown error while configuring Python import paths",
            "InitializePythonRuntime");
    }
    return result;
}

void ShutdownPythonRuntime() {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    if (!s_python_initialized || s_live_wrappers != 0) {
        return;
    }

    const bool finalize = s_runtime_owns_interpreter && Py_IsInitialized();
    s_python_initialized = false;
    s_runtime_owns_interpreter = false;
    if (finalize) {
        PyGILState_Ensure();
        Py_FinalizeEx();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  DomainletWrapper
// ═══════════════════════════════════════════════════════════════════

DomainletWrapper::DomainletWrapper(std::string_view module_name,
                                   std::string_view class_name)
    : module_name_(module_name), class_name_(class_name) {
    if (module_name_.empty() || class_name_.empty()) {
        throw std::runtime_error("module and class names must not be empty");
    }

    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    if (!s_python_initialized || !Py_IsInitialized()) {
        throw std::runtime_error(
            "Python runtime not initialized — call InitializePythonRuntime first");
    }

    try {
        py::gil_scoped_acquire gil;
        py::object module = py::module_::import_(module_name_.c_str());
        py::object type = module.attr(class_name_.c_str());
        instance_ = type();
        valid_ = true;
        registered_with_runtime_ = true;
        ++s_live_wrappers;
    } catch (const py::python_error& error) {
        std::ostringstream message;
        message << "failed to load domainlet [" << module_name_ << "."
                << class_name_ << "]: " << error.what();
        throw std::runtime_error(message.str());
    }
}

DomainletWrapper::~DomainletWrapper() {
    if (valid_) {
        (void) Teardown();
    }

    if (Py_IsInitialized()) {
        py::gil_scoped_acquire gil;
        instance_ = py::object();
    } else {
        // A host that finalizes Python before destroying a wrapper violates
        // the C ABI lifecycle contract. Avoid a decref against a dead VM.
        (void) instance_.release();
    }

    if (registered_with_runtime_) {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        if (s_live_wrappers > 0) {
            --s_live_wrappers;
        }
        registered_with_runtime_ = false;
    }
}

DomainletWrapper::DomainletWrapper(DomainletWrapper&& other) noexcept
    : module_name_(std::move(other.module_name_)),
      class_name_(std::move(other.class_name_)),
      instance_(std::move(other.instance_)),
      valid_(other.valid_),
      registered_with_runtime_(other.registered_with_runtime_) {
    other.valid_ = false;
    other.registered_with_runtime_ = false;
}

DomainletWrapper& DomainletWrapper::operator=(DomainletWrapper&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (valid_) {
        (void) Teardown();
    }
    if (Py_IsInitialized()) {
        py::gil_scoped_acquire gil;
        instance_ = py::object();
    } else {
        (void) instance_.release();
    }
    if (registered_with_runtime_) {
        std::lock_guard<std::mutex> lock(s_runtime_mutex);
        if (s_live_wrappers > 0) {
            --s_live_wrappers;
        }
    }

    module_name_ = std::move(other.module_name_);
    class_name_ = std::move(other.class_name_);
    instance_ = std::move(other.instance_);
    valid_ = other.valid_;
    registered_with_runtime_ = other.registered_with_runtime_;
    other.valid_ = false;
    other.registered_with_runtime_ = false;
    return *this;
}

// ── lifecycle methods ──────────────────────────────────────────────

BindletResult<DomainletInfo> DomainletWrapper::Probe() {
    BindletResult<DomainletInfo> result;
    if (!valid_) {
        result.error = internal_error("bindlet handle has been torn down", class_name_);
        return result;
    }

    try {
        py::gil_scoped_acquire gil;
        py::object py_info = instance_.attr("probe")();

        DomainletInfo& info = result.value;
        info.name = py::cast<std::string>(field(py_info, "name"));
        info.version = py::cast<std::string>(field(py_info, "version"));
        if (has_field(py_info, "description")) {
            info.description = py::cast<std::string>(field(py_info, "description"));
        }
        if (has_field(py_info, "api_version")) {
            info.api_version = py::cast<int>(field(py_info, "api_version"));
        }
        if (has_field(py_info, "author")) {
            info.author = py::cast<std::string>(field(py_info, "author"));
        }

        if (has_field(py_info, "capabilities")) {
            py::iterable capabilities =
                py::cast<py::iterable>(field(py_info, "capabilities"));
            for (py::handle py_capability : capabilities) {
                DomainletCapability capability;
                capability.name =
                    py::cast<std::string>(field(py_capability, "name"));
                if (has_field(py_capability, "description")) {
                    capability.description = py::cast<std::string>(
                        field(py_capability, "description"));
                }
                if (has_field(py_capability, "input_kind")) {
                    capability.input_kind =
                        input_kind_from_python(field(py_capability, "input_kind"));
                }
                if (has_field(py_capability, "output_kinds")) {
                    py::iterable output_kinds = py::cast<py::iterable>(
                        field(py_capability, "output_kinds"));
                    for (py::handle kind : output_kinds) {
                        capability.output_kinds.push_back(
                            py::cast<std::string>(kind));
                    }
                }
                if (has_field(py_capability, "tags")) {
                    py::iterable tags =
                        py::cast<py::iterable>(field(py_capability, "tags"));
                    for (py::handle tag : tags) {
                        capability.tags.push_back(py::cast<std::string>(tag));
                    }
                }
                info.capabilities.push_back(std::move(capability));
            }
        }
    } catch (const py::python_error& error) {
        result.error = _translate_py_error(error);
    } catch (const std::exception& error) {
        result.error = internal_error(
            std::string("malformed probe result: ") + error.what(), class_name_);
    } catch (...) {
        result.error = internal_error("malformed probe result", class_name_);
    }
    return result;
}

BindletResult<void> DomainletWrapper::Configure(
    const std::unordered_map<std::string, std::string>& config) {
    BindletResult<void> result;
    if (!valid_) {
        result.error = internal_error("bindlet handle has been torn down", class_name_);
        return result;
    }

    try {
        py::gil_scoped_acquire gil;
        py::dict py_config;
        for (const auto& [key, value] : config) {
            py_config[py::str(key.c_str())] = py::str(value.c_str());
        }
        instance_.attr("configure")(py_config);
    } catch (const py::python_error& error) {
        result.error = _translate_py_error(error);
    } catch (const std::exception& error) {
        result.error = internal_error(error.what(), class_name_);
    } catch (...) {
        result.error = internal_error("unknown configuration error", class_name_);
    }
    return result;
}

BindletResult<std::vector<AnalysisIssue>> DomainletWrapper::Execute(
    const AnalysisContext& context) {
    BindletResult<std::vector<AnalysisIssue>> result;
    if (!valid_) {
        result.error = internal_error("bindlet handle has been torn down", class_name_);
        return result;
    }

    try {
        py::gil_scoped_acquire gil;
        py::module_ bindlet_base = py::module_::import_("domainlet.bindlet_base");
        py::object input_kind;
        switch (context.kind) {
            case InputKind::TOKEN_STREAM:
                input_kind = bindlet_base.attr("InputKind").attr("TOKEN_STREAM");
                break;
            case InputKind::SOURCE_TEXT:
                input_kind = bindlet_base.attr("InputKind").attr("SOURCE_TEXT");
                break;
            case InputKind::CFG:
                input_kind = bindlet_base.attr("InputKind").attr("CFG");
                break;
            case InputKind::AST_JSON:
            default:
                input_kind = bindlet_base.attr("InputKind").attr("AST_JSON");
                break;
        }

        py::dict defines;
        for (const auto& [key, value] : context.defines) {
            defines[py::str(key.c_str())] = py::str(value.c_str());
        }
        py::list include_paths;
        for (const std::string& path : context.include_paths) {
            include_paths.append(py::str(path.c_str()));
        }
        py::dict options;
        for (const auto& [key, value] : context.options) {
            options[py::str(key.c_str())] = py::str(value.c_str());
        }

        py::object py_context = bindlet_base.attr("AnalysisContext")(
            py::arg("kind") = input_kind,
            py::arg("data") = py::str(context.data.c_str()),
            py::arg("source_file") = py::str(context.source_file.c_str()),
            py::arg("language") = py::str(context.language.c_str()),
            py::arg("defines") = defines,
            py::arg("include_paths") = include_paths,
            py::arg("options") = options);

        py::iterable py_issues =
            py::cast<py::iterable>(instance_.attr("execute")(py_context));
        for (py::handle py_issue : py_issues) {
            result.value.push_back(parse_issue(py_issue));
        }
    } catch (const py::python_error& error) {
        result.error = _translate_py_error(error);
    } catch (const std::exception& error) {
        result.error = internal_error(
            std::string("malformed execute result: ") + error.what(), class_name_);
    } catch (...) {
        result.error = internal_error("malformed execute result", class_name_);
    }
    return result;
}

BindletResult<void> DomainletWrapper::Teardown() {
    BindletResult<void> result;
    if (!valid_) {
        return result;
    }

    try {
        py::gil_scoped_acquire gil;
        instance_.attr("teardown")();
    } catch (const py::python_error& error) {
        result.error = _translate_py_error(error);
    } catch (const std::exception& error) {
        result.error = internal_error(error.what(), class_name_);
    } catch (...) {
        result.error = internal_error("unknown teardown error", class_name_);
    }
    valid_ = false;
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  Error translation
// ═══════════════════════════════════════════════════════════════════

BindletError DomainletWrapper::_translate_py_error(
    const dompybind::python_error& error) {
    py::gil_scoped_acquire gil;
    BindletError translated = _parse_bindlet_error(error.value());
    if (translated.message.empty()) {
        translated.message = error.what();
    }
    return translated;
}

BindletError DomainletWrapper::_parse_bindlet_error(dompybind::handle exception) {
    BindletError error = internal_error({});
    try {
        if (exception.is_valid() && py::hasattr(exception, "code")) {
            error.code = checked_error_code(
                py::cast<int>(exception.attr("code")));
        }
        if (exception.is_valid() && py::hasattr(exception, "message")) {
            error.message = py::cast<std::string>(exception.attr("message"));
        } else {
            error.message = py::cast<std::string>(py::str(exception));
        }
        if (exception.is_valid() && py::hasattr(exception, "source")) {
            error.source = py::cast<std::string>(exception.attr("source"));
        }
        if (exception.is_valid() && py::hasattr(exception, "detail")) {
            error.detail = py::cast<std::string>(exception.attr("detail"));
        }
    } catch (...) {
        error.code = ErrorCode::INTERNAL;
        error.message = "failed to translate Python exception";
    }
    return error;
}

}  // namespace bindlets
