/**
 * bindlets_api.cpp — C ABI implementation delegating to C++ DomainletWrapper
 */

#include "bindlets/bindlets.h"
#include "bindlets/domainlet_wrapper.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

struct bindlet_t {
    bindlets::DomainletWrapper wrapper;

    bindlet_t(std::string_view module, std::string_view class_name)
        : wrapper(module, class_name) {}
};

namespace {

template <typename T>
bool allocation_size_is_valid(std::size_t count) {
    return count <= std::numeric_limits<std::size_t>::max() / sizeof(T);
}

char* duplicate_string(const std::string& value, bool* success) {
    char* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (result == nullptr) {
        *success = false;
        return nullptr;
    }
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

void free_string_array(char** values, int count) {
    if (values == nullptr) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        std::free(values[index]);
    }
    std::free(values);
}

void free_span(c_source_span_t* span) {
    if (span == nullptr) {
        return;
    }
    std::free(span->file);
    *span = {};
}

void free_fix(c_fix_suggestion_t* fix) {
    if (fix == nullptr) {
        return;
    }
    std::free(fix->description);
    std::free(fix->replacement);
    free_span(&fix->span);
    *fix = {};
}

void free_capability(c_domainlet_capability_t* capability) {
    if (capability == nullptr) {
        return;
    }
    std::free(capability->name);
    std::free(capability->description);
    free_string_array(
        capability->output_kinds, capability->output_kind_count);
    free_string_array(capability->tags, capability->tag_count);
    *capability = {};
}

void free_info_fields(c_domainlet_info_t* info) {
    if (info == nullptr) {
        return;
    }
    std::free(info->name);
    std::free(info->version);
    std::free(info->description);
    std::free(info->author);
    if (info->capabilities != nullptr) {
        for (int index = 0; index < info->capability_count; ++index) {
            free_capability(&info->capabilities[index]);
        }
    }
    std::free(info->capabilities);
    *info = {};
}

void free_issue(c_analysis_issue_t* issue) {
    if (issue == nullptr) {
        return;
    }
    std::free(issue->rule_id);
    std::free(issue->message);
    free_span(&issue->location);
    std::free(issue->cwe_ids);
    free_fix(&issue->fix);
    free_string_array(issue->extra_keys, issue->extra_count);
    free_string_array(issue->extra_values, issue->extra_count);
    *issue = {};
}

bool make_span(const bindlets::SourceSpan& source, c_source_span_t* output) {
    bool success = true;
    *output = {};
    output->file = duplicate_string(source.file, &success);
    if (!success) {
        free_span(output);
        return false;
    }
    output->line_start = source.line_start;
    output->line_end = source.line_end;
    output->col_start = source.col_start;
    output->col_end = source.col_end;
    return true;
}

bool make_string_array(const std::vector<std::string>& source,
                       char*** output, int* count) {
    *output = nullptr;
    *count = 0;
    if (source.empty()) {
        return true;
    }
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || !allocation_size_is_valid<char*>(source.size())) {
        return false;
    }

    char** values = static_cast<char**>(
        std::calloc(source.size(), sizeof(char*)));
    if (values == nullptr) {
        return false;
    }

    bool success = true;
    for (std::size_t index = 0; index < source.size(); ++index) {
        values[index] = duplicate_string(source[index], &success);
        if (!success) {
            free_string_array(values, static_cast<int>(source.size()));
            return false;
        }
    }
    *output = values;
    *count = static_cast<int>(source.size());
    return true;
}

bool make_string_map(
    const std::unordered_map<std::string, std::string>& source,
    char*** keys, char*** values, int* count) {
    std::vector<std::pair<std::string, std::string>> entries(
        source.begin(), source.end());
    std::sort(entries.begin(), entries.end());

    std::vector<std::string> key_values;
    std::vector<std::string> mapped_values;
    key_values.reserve(entries.size());
    mapped_values.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        key_values.push_back(key);
        mapped_values.push_back(value);
    }

    int key_count = 0;
    int value_count = 0;
    if (!make_string_array(key_values, keys, &key_count)) {
        return false;
    }
    if (!make_string_array(mapped_values, values, &value_count)) {
        free_string_array(*keys, key_count);
        *keys = nullptr;
        return false;
    }
    if (key_count != value_count) {
        free_string_array(*keys, key_count);
        free_string_array(*values, value_count);
        *keys = nullptr;
        *values = nullptr;
        return false;
    }
    *count = key_count;
    return true;
}

bool make_fix(const bindlets::FixSuggestion& source,
              c_fix_suggestion_t* output) {
    bool success = true;
    *output = {};
    output->description = duplicate_string(source.description, &success);
    output->replacement = duplicate_string(source.replacement, &success);
    if (!success) {
        free_fix(output);
        return false;
    }
    if (source.span.has_value()) {
        if (!make_span(*source.span, &output->span)) {
            free_fix(output);
            return false;
        }
        output->has_span = 1;
    }
    return true;
}

bool make_issue(const bindlets::AnalysisIssue& source,
                c_analysis_issue_t* output) {
    bool success = true;
    *output = {};
    output->rule_id = duplicate_string(source.rule_id, &success);
    output->message = duplicate_string(source.message, &success);
    if (!success || !make_span(source.location, &output->location)) {
        free_issue(output);
        return false;
    }
    output->severity = static_cast<c_severity_t>(source.severity);
    output->confidence = static_cast<c_confidence_t>(source.confidence);

    if (!source.cwe_ids.empty()) {
        if (source.cwe_ids.size()
                > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || !allocation_size_is_valid<int>(source.cwe_ids.size())) {
            free_issue(output);
            return false;
        }
        output->cwe_ids = static_cast<int*>(
            std::malloc(sizeof(int) * source.cwe_ids.size()));
        if (output->cwe_ids == nullptr) {
            free_issue(output);
            return false;
        }
        std::memcpy(output->cwe_ids, source.cwe_ids.data(),
                    sizeof(int) * source.cwe_ids.size());
        output->cwe_count = static_cast<int>(source.cwe_ids.size());
    }

    if (source.fix.has_value()) {
        if (!make_fix(*source.fix, &output->fix)) {
            free_issue(output);
            return false;
        }
        output->has_fix = 1;
    }
    if (!make_string_map(
            source.extra, &output->extra_keys, &output->extra_values,
            &output->extra_count)) {
        free_issue(output);
        return false;
    }
    return true;
}

bool make_capability(const bindlets::DomainletCapability& source,
                     c_domainlet_capability_t* output) {
    bool success = true;
    *output = {};
    output->name = duplicate_string(source.name, &success);
    output->description = duplicate_string(source.description, &success);
    if (!success
        || !make_string_array(source.output_kinds, &output->output_kinds,
                              &output->output_kind_count)
        || !make_string_array(source.tags, &output->tags, &output->tag_count)) {
        free_capability(output);
        return false;
    }
    output->input_kind = static_cast<int>(source.input_kind);
    return true;
}

bool make_info(const bindlets::DomainletInfo& source,
               c_domainlet_info_t* output) {
    bool success = true;
    *output = {};
    output->name = duplicate_string(source.name, &success);
    output->version = duplicate_string(source.version, &success);
    output->description = duplicate_string(source.description, &success);
    output->author = duplicate_string(source.author, &success);
    if (!success) {
        free_info_fields(output);
        return false;
    }
    output->api_version = source.api_version;

    if (source.capabilities.empty()) {
        return true;
    }
    if (source.capabilities.size()
            > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || !allocation_size_is_valid<c_domainlet_capability_t>(
            source.capabilities.size())) {
        free_info_fields(output);
        return false;
    }
    output->capabilities = static_cast<c_domainlet_capability_t*>(
        std::calloc(source.capabilities.size(), sizeof(c_domainlet_capability_t)));
    if (output->capabilities == nullptr) {
        free_info_fields(output);
        return false;
    }
    output->capability_count = static_cast<int>(source.capabilities.size());
    for (int index = 0; index < output->capability_count; ++index) {
        if (!make_capability(source.capabilities[index],
                             &output->capabilities[index])) {
            free_info_fields(output);
            return false;
        }
    }
    return true;
}

bool make_error(const bindlets::BindletError& source,
                c_bindlet_error_detail_t** output) {
    *output = nullptr;
    c_bindlet_error_detail_t* error = static_cast<c_bindlet_error_detail_t*>(
        std::calloc(1, sizeof(c_bindlet_error_detail_t)));
    if (error == nullptr) {
        return false;
    }

    bool success = true;
    error->code = static_cast<c_bindlet_error_t>(source.code);
    error->message = duplicate_string(source.message, &success);
    error->source = duplicate_string(source.source, &success);
    error->detail = duplicate_string(source.detail, &success);
    if (!success) {
        bindlets_free_error(error);
        return false;
    }
    *output = error;
    return true;
}

bool valid_parallel_arrays(const char** keys, const char** values, int count) {
    return count >= 0 && (count == 0 || (keys != nullptr && values != nullptr));
}

bool valid_array(const char** values, int count) {
    return count >= 0 && (count == 0 || values != nullptr);
}

int report_operation_error(const bindlets::BindletError& error,
                           c_bindlet_error_detail_t** output) {
    if (output != nullptr && !make_error(error, output)) {
        return BINDLET_ERR_INTERNAL;
    }
    return static_cast<int>(error.code);
}

}  // namespace

int bindlets_init_runtime(const char* domainlet_path) {
    const std::string path = domainlet_path != nullptr ? domainlet_path : ".";
    const auto result = bindlets::InitializePythonRuntime(path);
    return static_cast<int>(result.error.code);
}

void bindlets_shutdown_runtime(void) {
    bindlets::ShutdownPythonRuntime();
}

int bindlets_create(bindlet_t** handle, const char* module_name,
                    const char* class_name) {
    if (handle != nullptr) {
        *handle = nullptr;
    }
    if (handle == nullptr || module_name == nullptr || class_name == nullptr
        || module_name[0] == '\0' || class_name[0] == '\0') {
        return BINDLET_ERR_NOT_FOUND;
    }

    try {
        *handle = new bindlet_t(module_name, class_name);
        return BINDLET_OK;
    } catch (const std::bad_alloc&) {
        return BINDLET_ERR_INTERNAL;
    } catch (const std::exception&) {
        return BINDLET_ERR_NOT_FOUND;
    } catch (...) {
        return BINDLET_ERR_INTERNAL;
    }
}

void bindlets_destroy(bindlet_t* handle) {
    delete handle;
}

int bindlets_probe(bindlet_t* handle, c_domainlet_info_t** info) {
    if (info != nullptr) {
        *info = nullptr;
    }
    if (handle == nullptr || info == nullptr) {
        return BINDLET_ERR_INTERNAL;
    }

    const auto result = handle->wrapper.Probe();
    if (!result.ok()) {
        return static_cast<int>(result.error.code);
    }

    c_domainlet_info_t* converted = static_cast<c_domainlet_info_t*>(
        std::calloc(1, sizeof(c_domainlet_info_t)));
    if (converted == nullptr || !make_info(result.value, converted)) {
        bindlets_free_info(converted);
        return BINDLET_ERR_INTERNAL;
    }
    *info = converted;
    return BINDLET_OK;
}

int bindlets_configure(bindlet_t* handle, const char** keys,
                       const char** values, int config_size,
                       c_bindlet_error_detail_t** err) {
    if (err != nullptr) {
        *err = nullptr;
    }
    if (handle == nullptr || !valid_parallel_arrays(keys, values, config_size)) {
        return BINDLET_ERR_INPUT_MALFORMED;
    }

    std::unordered_map<std::string, std::string> config;
    for (int index = 0; index < config_size; ++index) {
        if (keys[index] != nullptr && values[index] != nullptr) {
            config[keys[index]] = values[index];
        }
    }

    const auto result = handle->wrapper.Configure(config);
    if (!result.ok()) {
        return report_operation_error(result.error, err);
    }
    return BINDLET_OK;
}

int bindlets_execute(bindlet_t* handle, const char* ast_json,
                     const char* source_file, const char* language,
                     const char** define_keys, const char** define_values,
                     int define_count, const char** include_paths,
                     int include_count, const char** option_keys,
                     const char** option_values, int option_count,
                     c_analysis_issue_t** issues, int* count,
                     c_bindlet_error_detail_t** err) {
    if (issues != nullptr) {
        *issues = nullptr;
    }
    if (count != nullptr) {
        *count = 0;
    }
    if (err != nullptr) {
        *err = nullptr;
    }
    if (handle == nullptr || issues == nullptr || count == nullptr
        || !valid_parallel_arrays(define_keys, define_values, define_count)
        || !valid_array(include_paths, include_count)
        || !valid_parallel_arrays(option_keys, option_values, option_count)) {
        return BINDLET_ERR_INPUT_MALFORMED;
    }

    bindlets::AnalysisContext context;
    context.data = ast_json != nullptr ? ast_json : "";
    context.source_file = source_file != nullptr ? source_file : "";
    context.language = language != nullptr ? language : "c";
    for (int index = 0; index < define_count; ++index) {
        if (define_keys[index] != nullptr && define_values[index] != nullptr) {
            context.defines[define_keys[index]] = define_values[index];
        }
    }
    for (int index = 0; index < include_count; ++index) {
        if (include_paths[index] != nullptr) {
            context.include_paths.emplace_back(include_paths[index]);
        }
    }
    for (int index = 0; index < option_count; ++index) {
        if (option_keys[index] != nullptr && option_values[index] != nullptr) {
            context.options[option_keys[index]] = option_values[index];
        }
    }

    const auto result = handle->wrapper.Execute(context);
    if (!result.ok()) {
        return report_operation_error(result.error, err);
    }
    if (result.value.empty()) {
        return BINDLET_OK;
    }
    if (result.value.size()
            > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || !allocation_size_is_valid<c_analysis_issue_t>(result.value.size())) {
        return BINDLET_ERR_INTERNAL;
    }

    c_analysis_issue_t* converted = static_cast<c_analysis_issue_t*>(
        std::calloc(result.value.size(), sizeof(c_analysis_issue_t)));
    if (converted == nullptr) {
        return BINDLET_ERR_INTERNAL;
    }
    const int converted_count = static_cast<int>(result.value.size());
    for (int index = 0; index < converted_count; ++index) {
        if (!make_issue(result.value[index], &converted[index])) {
            bindlets_free_issues(converted, converted_count);
            return BINDLET_ERR_INTERNAL;
        }
    }
    *issues = converted;
    *count = converted_count;
    return BINDLET_OK;
}

void bindlets_free_info(c_domainlet_info_t* info) {
    if (info == nullptr) {
        return;
    }
    free_info_fields(info);
    std::free(info);
}

void bindlets_free_issues(c_analysis_issue_t* issues, int count) {
    if (issues == nullptr) {
        return;
    }
    if (count > 0) {
        for (int index = 0; index < count; ++index) {
            free_issue(&issues[index]);
        }
    }
    std::free(issues);
}

void bindlets_free_error(c_bindlet_error_detail_t* err) {
    if (err == nullptr) {
        return;
    }
    std::free(err->message);
    std::free(err->source);
    std::free(err->detail);
    std::free(err);
}

const char* bindlets_error_string(c_bindlet_error_t code) {
    switch (code) {
        case BINDLET_OK:
            return "OK";
        case BINDLET_ERR_NOT_FOUND:
            return "NOT_FOUND";
        case BINDLET_ERR_CONFIG_INVALID:
            return "CONFIG_INVALID";
        case BINDLET_ERR_VERSION_MISMATCH:
            return "VERSION_MISMATCH";
        case BINDLET_ERR_EXECUTION_FAILED:
            return "EXECUTION_FAILED";
        case BINDLET_ERR_INPUT_MALFORMED:
            return "INPUT_MALFORMED";
        case BINDLET_ERR_TIMEOUT:
            return "TIMEOUT";
        case BINDLET_ERR_INTERNAL:
            return "INTERNAL";
        default:
            return "UNKNOWN";
    }
}

int bindlets_abi_version(void) {
    return 1;
}
