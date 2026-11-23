#include "sdk_internal.hpp"

#include "cephyr/diagnostics.hpp"
#include "cephyr/target.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cephyr {
namespace {

bool ascii_alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

bool span_ok(cephyr_sdk_text text, std::size_t maximum) {
    return text.size <= maximum &&
           (!text.size || (text.data && !std::memchr(text.data, '\0', text.size)));
}

std::string_view view(cephyr_sdk_text text) {
    return text.size ? std::string_view(text.data, text.size) : std::string_view{};
}

cephyr_sdk_text span(std::string_view text) {
    return {text.data(), text.size()};
}

bool valid_name(cephyr_sdk_text text) {
    if (!text.size || !span_ok(text, CEPHYR_SDK_MAX_NAME_BYTES))
        return false;
    return std::ranges::all_of(view(text), [](char c) {
        return ascii_alnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool valid_directive_name(cephyr_sdk_text text) {
    if (!text.size || !span_ok(text, CEPHYR_SDK_MAX_NAME_BYTES))
        return false;
    const auto name = view(text);
    return !(name.front() >= '0' && name.front() <= '9') &&
           std::ranges::all_of(name, [](char c) { return ascii_alnum(c) || c == '_'; });
}

bool valid_triple(cephyr_sdk_text text) {
    if (!span_ok(text, CEPHYR_SDK_MAX_TRIPLE_BYTES))
        return false;
    if (!text.size)
        return true;
    const std::string_view triple = view(text);
    std::size_t start = 0;
    unsigned components = 0;
    do {
        const auto end = triple.find('-', start);
        const auto part = triple.substr(start, end == triple.npos ? end : end - start);
        if (part.empty() || !ascii_alnum(part.front()) || !ascii_alnum(part.back()) ||
            !std::ranges::all_of(part, [](char c) {
                return ascii_alnum(c) || c == '_' || c == '.' || c == '+';
            }))
            return false;
        ++components;
        if (end == triple.npos)
            break;
        start = end + 1;
    } while (true);
    return components >= 3 && components <= 4;
}

bool matches(std::string_view selector, const Target &target) {
    return selector.empty() || selector == target.triple;
}

std::string_view stage_name(uint32_t stage) {
    switch (stage) {
    case CEPHYR_SDK_DESUGAR:
        return "desugar";
    case CEPHYR_SDK_ANALYZE:
        return "analysis";
    case CEPHYR_SDK_REWRITE:
        return "rewrite";
    case CEPHYR_SDK_ASSEMBLY:
        return "assembly";
    case CEPHYR_SDK_DIRECTIVE:
        return "directive";
    }
    return "unknown";
}

struct Invocation {
    Diagnostics &diagnostics;
    std::string_view input_name;
    std::size_t &diagnostic_count;
    uint32_t stage;
    uint32_t line;
    uint32_t column;
    std::size_t remaining_directive_output;
    std::optional<std::string> replacement;
    const char *failure = nullptr;

    int fail(const char *message) noexcept {
        if (!failure)
            failure = message;
        return 0;
    }
};

// Do not interpret a client-supplied context as a pointer to live storage
// until it matches the active callback on this thread.
thread_local Invocation *active_invocation = nullptr;

class InvocationScope {
public:
    explicit InvocationScope(Invocation &call) noexcept
        : previous_(std::exchange(active_invocation, &call)) {}
    ~InvocationScope() { active_invocation = previous_; }
    InvocationScope(const InvocationScope &) = delete;
    InvocationScope &operator=(const InvocationScope &) = delete;

private:
    Invocation *previous_;
};

Invocation *checked_invocation(void *context) noexcept {
    auto *call = active_invocation;
    if (call && context != call) {
        call->fail("invalid callback sink context");
        return nullptr;
    }
    return call;
}

int set_text(void *context, cephyr_sdk_text text) noexcept {
    auto *active = checked_invocation(context);
    if (!active)
        return 0;
    auto &call = *active;
    if (call.stage == CEPHYR_SDK_ANALYZE)
        return call.fail("analysis hooks cannot replace text");
    if (call.replacement)
        return call.fail("text may be submitted only once per callback");
    const bool directive = call.stage == CEPHYR_SDK_DIRECTIVE;
    const auto maximum = directive                       ? CEPHYR_SDK_MAX_DIRECTIVE_OUTPUT_BYTES :
                         call.stage == CEPHYR_SDK_DESUGAR  ? CEPHYR_SDK_MAX_SOURCE_BYTES :
                         call.stage == CEPHYR_SDK_REWRITE  ? CEPHYR_SDK_MAX_MIR_BYTES :
                                                              CEPHYR_SDK_MAX_ASSEMBLY_BYTES;
    if (text.size > maximum)
        return call.fail("text exceeds the stage output limit");
    if (directive && text.size > call.remaining_directive_output)
        return call.fail("total directive output limit exceeded");
    if (!span_ok(text, maximum) || (!text.size && !directive && call.stage != CEPHYR_SDK_DESUGAR))
        return call.fail("invalid text output");
    if (call.stage == CEPHYR_SDK_ASSEMBLY && text.data[text.size - 1] != '\n')
        return call.fail("assembly text must end in a newline");
    try {
        if (directive && !valid_directive_output(view(text)))
            return call.fail("directive output must be already-preprocessed C text");
        call.replacement.emplace(view(text));
        return 1;
    } catch (...) {
        return call.fail("unable to copy callback output");
    }
}

int report(void *context, uint32_t severity, cephyr_sdk_text code,
           cephyr_sdk_text message, cephyr_sdk_text file,
           uint32_t line, uint32_t column) noexcept {
    auto *active = checked_invocation(context);
    if (!active)
        return 0;
    auto &call = *active;
    if (call.diagnostic_count >= CEPHYR_SDK_MAX_DIAGNOSTICS)
        return call.fail("diagnostic limit exceeded");
    if ((severity != CEPHYR_SDK_NOTE && severity != CEPHYR_SDK_WARNING &&
         severity != CEPHYR_SDK_ERROR) ||
        !valid_name(code) || !message.size ||
        !span_ok(message, CEPHYR_SDK_MAX_MESSAGE_BYTES) ||
        !span_ok(file, CEPHYR_SDK_MAX_FILE_BYTES) || (column && !line))
        return call.fail("invalid diagnostic");
    try {
        if (call.stage == CEPHYR_SDK_DIRECTIVE && !file.size && !line && !column) {
            line = call.line;
            column = call.column;
        }
        Location where{std::string(file.size ? view(file) : call.input_name), line, column};
        switch (severity) {
        case CEPHYR_SDK_NOTE:
            call.diagnostics.note(view(code), view(message), std::move(where));
            break;
        case CEPHYR_SDK_WARNING:
            call.diagnostics.warning(view(code), view(message), std::move(where));
            break;
        case CEPHYR_SDK_ERROR:
            call.diagnostics.error(view(code), view(message), std::move(where));
            break;
        }
        ++call.diagnostic_count;
        return 1;
    } catch (...) {
        return call.fail("unable to copy callback diagnostic");
    }
}

} // namespace

bool SdkPipeline::validate(Diagnostics &diagnostics) {
    valid_ = false;
    hooks_.clear();
    directives_.clear();
    diagnostic_count_ = 0;
    directive_invocations_ = 0;
    directive_output_bytes_ = 0;
    if (!sdk_) {
        valid_ = true;
        return true;
    }

    constexpr auto v1_size = offsetof(cephyr_sdk, directives);
    constexpr auto directive_fields_size = offsetof(cephyr_sdk, directive_count) +
                                           sizeof(cephyr_sdk::directive_count);
    if (sdk_->struct_size < v1_size || sdk_->abi_version != CEPHYR_SDK_ABI_VERSION) {
        diagnostics.error("cephyr.sdk.registration", "incompatible SDK descriptor size or ABI version");
        return false;
    }
    // The pointer and count form a pair. Older clients need not allocate
    // storage past the original prefix at all.
    const auto directive_count = sdk_->struct_size >= directive_fields_size ? sdk_->directive_count : 0;
    if (directive_count > CEPHYR_SDK_MAX_DIRECTIVES || (directive_count && !sdk_->directives)) {
        diagnostics.error("cephyr.sdk.registration", "invalid SDK directive count or missing directive array");
        return false;
    }
    if (sdk_->hook_count > CEPHYR_SDK_MAX_HOOKS || (sdk_->hook_count && !sdk_->hooks)) {
        diagnostics.error("cephyr.sdk.registration", "invalid SDK hook count or missing hook array");
        return false;
    }

    std::vector<Hook> hooks;
    hooks.reserve(sdk_->hook_count);
    for (std::size_t i = 0; i < sdk_->hook_count; ++i) {
        const auto &hook = sdk_->hooks[i];
        const auto invalid = [&](std::string_view reason) {
            diagnostics.error("cephyr.sdk.registration",
                              "SDK hook " + std::to_string(i) + ": " + std::string(reason));
            return false;
        };
        if (hook.stage < CEPHYR_SDK_DESUGAR || hook.stage > CEPHYR_SDK_ASSEMBLY)
            return invalid("unknown stage");
        if (!hook.callback || !valid_name(hook.name))
            return invalid("missing callback or invalid hook name");
        if (!valid_triple(hook.target_triple))
            return invalid("invalid target triple");
        if (hook.stage == CEPHYR_SDK_ASSEMBLY && !hook.target_triple.size)
            return invalid("an assembly hook requires an exact target triple");

        const auto name = view(hook.name);
        const auto triple = view(hook.target_triple);
        for (const auto &prior : hooks) {
            if (name == prior.name)
                return invalid("hook name must be unique");
            if (hook.stage == CEPHYR_SDK_ASSEMBLY && prior.stage == hook.stage &&
                triple == prior.target_triple)
                return invalid("more than one assembly hook claims this target");
        }
        hooks.push_back({hook.stage, std::string(name), std::string(triple),
                         hook.callback, hook.user});
    }

    std::vector<Hook> directives;
    directives.reserve(directive_count);
    for (std::size_t i = 0; i < directive_count; ++i) {
        const auto &entry = sdk_->directives[i];
        const auto invalid = [&](std::string_view reason) {
            diagnostics.error("cephyr.sdk.registration",
                              "SDK directive " + std::to_string(i) + ": " + std::string(reason));
            return false;
        };
        if (!entry.callback || !valid_directive_name(entry.name))
            return invalid("missing callback or invalid directive name");
        if (!valid_triple(entry.target_triple))
            return invalid("invalid target triple");
        const auto name = view(entry.name);
        const auto triple = view(entry.target_triple);
        for (const auto &prior : directives)
            if (name == prior.name && triple == prior.target_triple)
                return invalid("directive name and target pair must be unique");
        directives.push_back({CEPHYR_SDK_DIRECTIVE, std::string(name), std::string(triple),
                              entry.callback, entry.user});
    }
    hooks_.swap(hooks);
    directives_.swap(directives);
    valid_ = true;
    return true;
}

bool SdkPipeline::ready(Diagnostics &diagnostics) const {
    if (!valid_) {
        diagnostics.error("cephyr.sdk.registration", "SDK pipeline was not validated");
        return false;
    }
    return true;
}

const SdkPipeline::Hook *SdkPipeline::find_backend(const Target &target) const {
    for (const auto &hook : hooks_)
        if (hook.stage == CEPHYR_SDK_ASSEMBLY && hook.target_triple == target.triple)
            return &hook;
    return nullptr;
}

bool SdkPipeline::invoke(const Hook &hook, std::string_view text, const Target &target,
                         std::string_view input_name, uint32_t flags, uint32_t line,
                         uint32_t column, Diagnostics &diagnostics,
                         std::optional<std::string> &replacement) {
    const bool directive = hook.stage == CEPHYR_SDK_DIRECTIVE;
    const cephyr_sdk_input input{
        sizeof(cephyr_sdk_input), CEPHYR_SDK_ABI_VERSION, hook.stage,
        span(text), span(input_name), span(target.triple), span(target.architecture),
        span(target.abi), directive || hook.stage == CEPHYR_SDK_DESUGAR ? 0u : CEPHYR_SDK_MIR_SCHEMA_VERSION,
        flags, directive ? span(hook.name) : cephyr_sdk_text{}, line, column
    };
    Invocation call{diagnostics, input_name, diagnostic_count_, hook.stage, line, column,
                    CEPHYR_SDK_MAX_SOURCE_BYTES - directive_output_bytes_, std::nullopt};
    const cephyr_sdk_output output{
        sizeof(cephyr_sdk_output), CEPHYR_SDK_ABI_VERSION, &call, set_text, report
    };
    const auto errors_before = diagnostics.error_count();
    const auto error = [&](std::string_view reason) {
        diagnostics.error("cephyr.sdk.callback",
                          std::string(stage_name(hook.stage)) + (directive ? " '" : " hook '") +
                              hook.name + "': " +
                              std::string(reason),
                          Location{std::string(input_name), line, column});
        return false;
    };

    uint32_t status;
    try {
        const InvocationScope scope(call);
        status = hook.callback(&input, &output, hook.user);
    } catch (...) {
        return error("callback threw an exception");
    }
    if (call.failure)
        return error(call.failure);
    if (status != CEPHYR_SDK_OK && status != CEPHYR_SDK_REJECT)
        return error("unknown callback status");
    if (diagnostics.error_count() != errors_before)
        return false;
    if (status == CEPHYR_SDK_REJECT)
        return error("callback rejected the input");
    if (hook.stage == CEPHYR_SDK_ASSEMBLY && !call.replacement)
        return error("callback did not submit assembly");

    replacement = std::move(call.replacement);
    return true;
}

bool SdkPipeline::transform(uint32_t stage, std::string &text, const Target &target,
                            std::string_view input_name, Diagnostics &diagnostics, bool *changed) {
    if (changed)
        *changed = false;
    if (!ready(diagnostics))
        return false;
    // Keep the original intact until the whole stage succeeds. Diagnostics from
    // any hook, including a rejecting hook, are retained.
    std::optional<std::string> current;
    for (const auto &hook : hooks_) {
        if (hook.stage != stage || !matches(hook.target_triple, target))
            continue;
        std::optional<std::string> next;
        if (!invoke(hook, current ? std::string_view(*current) : std::string_view(text),
                    target, input_name, 0, 0, 0, diagnostics, next))
            return false;
        if (next)
            current = std::move(next);
    }
    if (current && *current != text) {
        text.swap(*current);
        if (changed)
            *changed = true;
    }
    return true;
}

bool SdkPipeline::desugar(std::string &source, const Target &target,
                          std::string_view input_name, Diagnostics &diagnostics) {
    return transform(CEPHYR_SDK_DESUGAR, source, target, input_name, diagnostics, nullptr);
}

std::vector<std::string> SdkPipeline::directive_names(const Target &target) const {
    std::vector<std::string> names;
    for (const auto &entry : directives_)
        if (matches(entry.target_triple, target) && std::ranges::find(names, entry.name) == names.end())
            names.push_back(entry.name);
    return names;
}

bool SdkPipeline::directive(std::string_view name, std::string_view arguments,
                            const Target &target, const Location &where, Diagnostics &diagnostics,
                            std::optional<std::string> &replacement) {
    if (!ready(diagnostics))
        return false;
    const Hook *handler = nullptr;
    for (const auto &entry : directives_) {
        if (entry.name != name)
            continue;
        if (entry.target_triple == target.triple) {
            handler = &entry;
            break;
        }
        if (entry.target_triple.empty())
            handler = &entry;
    }
    if (!handler) {
        diagnostics.error("cephyr.sdk.directive",
                          "unknown Cephyr directive '" + std::string(name) +
                              "' for target '" + target.triple + "'", where);
        return false;
    }
    if (directive_invocations_ >= CEPHYR_SDK_MAX_DIRECTIVE_INVOCATIONS) {
        diagnostics.error("cephyr.sdk.directive", "directive invocation limit exceeded", where);
        return false;
    }
    ++directive_invocations_;
    if (!invoke(*handler, arguments, target, where.file, 0, where.line, where.column,
                diagnostics, replacement))
        return false;
    if (replacement)
        directive_output_bytes_ += replacement->size();
    return true;
}

bool SdkPipeline::analyze(std::string_view mir, const Target &target,
                          std::string_view input_name, Diagnostics &diagnostics) {
    if (!ready(diagnostics))
        return false;
    for (const auto &hook : hooks_) {
        if (hook.stage != CEPHYR_SDK_ANALYZE || !matches(hook.target_triple, target))
            continue;
        std::optional<std::string> ignored;
        if (!invoke(hook, mir, target, input_name, 0, 0, 0, diagnostics, ignored))
            return false;
    }
    return true;
}

bool SdkPipeline::rewrite(std::string &mir, const Target &target,
                          std::string_view input_name, Diagnostics &diagnostics, bool *changed) {
    return transform(CEPHYR_SDK_REWRITE, mir, target, input_name, diagnostics, changed);
}

bool SdkPipeline::has_backend(const Target &target) const {
    return find_backend(target) != nullptr;
}

std::optional<std::string> SdkPipeline::emit_assembly(std::string_view mir, const Target &target,
                                                     std::string_view input_name, bool position_independent,
                                                     Diagnostics &diagnostics) {
    if (!ready(diagnostics))
        return std::nullopt;
    const auto *backend = find_backend(target);
    if (!backend) {
        diagnostics.error("cephyr.sdk.callback", "no SDK assembly hook matches this target",
                          Location{std::string(input_name)});
        return std::nullopt;
    }
    std::optional<std::string> assembly;
    if (!invoke(*backend, mir, target, input_name,
                position_independent ? static_cast<uint32_t>(CEPHYR_SDK_POSITION_INDEPENDENT) : 0u,
                0, 0, diagnostics, assembly))
        return std::nullopt;
    return assembly;
}

} // namespace cephyr
