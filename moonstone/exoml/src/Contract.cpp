#include <exoml/Contract.hpp>
#include "JsonReader.hpp"
#include <algorithm>
#include <stdexcept>

namespace moonstone::exoml::contract {
namespace {

// ── Enum string tables ─────────────────────────────────────────────────

template <class E, std::size_t N>
E parse_enum(const char *const (&names)[N], std::string_view text) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == text)
            return static_cast<E>(i);
    throw std::invalid_argument(std::string("unknown enum value: ") + std::string(text));
}
const char *enum_name_impl(const char *const *names, std::size_t count, unsigned index) {
    return index < count ? names[index] : "unknown";
}

#define ENUM_TABLE(TYPE, ...)                                                                      \
    static constexpr const char *TYPE##_names[] = {__VA_ARGS__};                                   \
    static constexpr std::size_t TYPE##_count = sizeof(TYPE##_names) / sizeof(TYPE##_names[0]);    \
    inline TYPE parse_##TYPE(std::string_view t) {                                                 \
        return parse_enum<TYPE, TYPE##_count>(TYPE##_names, t);                                    \
    }                                                                                              \
    inline const char *TYPE##_name(TYPE v) {                                                       \
        return enum_name_impl(TYPE##_names, TYPE##_count, static_cast<unsigned>(v));               \
    }

ENUM_TABLE(OwnershipKind, "scalar", "borrowed_immutable", "borrowed_mutable", "owned_unique",
           "shared", "consumed", "out", "inout", "opaque_unsafe", "retained_callback",
           "transferred")
ENUM_TABLE(LifetimeOrigin, "parameter", "receiver", "returned_owner", "global_static",
           "caller_buffer", "callback_registration", "explicit_region")
ENUM_TABLE(InvalidationKind, "owner_drop", "owner_move", "mutation", "reallocation",
           "registration_destruction", "next_call", "explicit_operation",
           "concurrency_dependent")
ENUM_TABLE(ReturnKind, "value", "borrow", "owned", "nullable", "tagged_error")
ENUM_TABLE(ErrorKind, "none", "sentinel", "null_on_failure", "boolean_return", "int_code",
           "errno", "out_error", "custom_classifier")
ENUM_TABLE(TransferBranch, "success", "failure", "unconditional", "conditional")
ENUM_TABLE(ContractState, "discovered", "annotated", "validated", "reviewed", "usage_checked")
ENUM_TABLE(EffectKind, "read", "write", "allocate", "free", "escape", "io", "thread", "suspend",
           "foreign_call")
ENUM_TABLE(ReviewStatus, "unreviewed", "auto_inferred", "reviewed", "verified")
ENUM_TABLE(ThreadSafety, "single_thread", "thread_safe", "thread_affine", "signal_safe")
ENUM_TABLE(CallbackMode, "synchronous", "retained")

// ── JSON helpers ─────────────────────────────────────────────────────────

using detail::JsonValue;
using detail::JsonType;

const JsonValue &require_array(const JsonValue &v, const char *field) {
    if (v.type != JsonType::Array)
        throw std::invalid_argument(std::string("expected array for field: ") + field);
    return v;
}
std::string opt_string(const JsonValue &v, const char *field) {
    if (!v.contains(field))
        return {};
    const auto &child = v[field];
    if (child.type == JsonType::Null)
        return {};
    return child.as_string();
}
std::uint32_t opt_uint(const JsonValue &v, const char *field) {
    if (!v.contains(field))
        return 0;
    const auto &child = v[field];
    if (child.type == JsonType::Null)
        return 0;
    return static_cast<std::uint32_t>(child.as_int());
}
std::optional<std::uint32_t> opt_optional_uint(const JsonValue &v, const char *field) {
    if (!v.contains(field))
        return std::nullopt;
    const auto &child = v[field];
    if (child.type == JsonType::Null)
        return std::nullopt;
    return static_cast<std::uint32_t>(child.as_int());
}
bool opt_bool(const JsonValue &v, const char *field) {
    if (!v.contains(field))
        return false;
    return v[field].as_bool();
}

SourceSpan parse_source_span(const JsonValue &v) {
    SourceSpan s;
    if (v.type != JsonType::Object)
        return s;
    s.file = opt_string(v, "file");
    s.line = opt_uint(v, "line");
    s.column = opt_uint(v, "column");
    s.end_line = opt_uint(v, "end_line");
    s.end_column = opt_uint(v, "end_column");
    return s;
}

LifetimeSpec parse_lifetime(const JsonValue &v) {
    LifetimeSpec spec;
    if (v.type != JsonType::Object)
        return spec;
    if (v.contains("origin"))
        spec.origin = parse_LifetimeOrigin(v["origin"].as_string());
    spec.parameter_index = opt_uint(v, "parameter_index");
    spec.region_name = opt_string(v, "region_name");
    return spec;
}

ReturnContract parse_return_contract(const JsonValue &v) {
    ReturnContract rc;
    if (v.type != JsonType::Object)
        return rc;
    if (v.contains("kind"))
        rc.kind = parse_ReturnKind(v["kind"].as_string());
    rc.nullable = opt_bool(v, "nullable");
    if (v.contains("lifetime"))
        rc.lifetime = parse_lifetime(v["lifetime"]);
    rc.destroy_with = opt_string(v, "destroy_with");
    rc.allocation_source = opt_string(v, "allocation_source");
    rc.deallocator_family = opt_string(v, "deallocator_family");
    rc.nullable_means_success = opt_bool(v, "nullable_means_success");
    rc.alias_of = opt_string(v, "alias_of");
    return rc;
}

ParameterContract parse_parameter_contract(const JsonValue &v) {
    ParameterContract pc;
    if (v.type != JsonType::Object)
        return pc;
    pc.index = opt_uint(v, "index");
    pc.name = opt_string(v, "name");
    pc.nullable = opt_bool(v, "nullable");
    if (v.contains("ownership"))
        pc.ownership = parse_OwnershipKind(v["ownership"].as_string());
    if (v.contains("lifetime"))
        pc.lifetime = parse_lifetime(v["lifetime"]);
    pc.length_parameter = opt_optional_uint(v, "length_parameter");
    pc.count_parameter = opt_optional_uint(v, "count_parameter");
    pc.encoding = opt_string(v, "encoding");
    pc.alignment = opt_optional_uint(v, "alignment");
    if (v.contains("transfer_branch"))
        pc.transfer_branch = parse_TransferBranch(v["transfer_branch"].as_string());
    pc.transfer_predicate = opt_string(v, "transfer_predicate");
    return pc;
}

ErrorContract parse_error_contract(const JsonValue &v) {
    ErrorContract ec;
    if (v.type != JsonType::Object)
        return ec;
    if (v.contains("kind"))
        ec.kind = parse_ErrorKind(v["kind"].as_string());
    ec.sentinel_value = opt_string(v, "sentinel_value");
    ec.out_valid_on_failure = opt_bool(v, "out_valid_on_failure");
    ec.cleanup_required_on_failure = opt_bool(v, "cleanup_required_on_failure");
    ec.error_parameter = opt_string(v, "error_parameter");
    return ec;
}

EffectContract parse_effect_contract(const JsonValue &v) {
    EffectContract ec;
    if (v.type != JsonType::Object)
        return ec;
    ec.unknown = opt_bool(v, "unknown");
    if (v.contains("effects")) {
        const auto &arr = require_array(v["effects"], "effects");
        for (std::size_t i = 0; i < arr.size(); ++i)
            ec.effects.insert(parse_EffectKind(arr[i].as_string()));
    }
    return ec;
}

ConcurrencyContract parse_concurrency_contract(const JsonValue &v) {
    ConcurrencyContract cc;
    if (v.type != JsonType::Object)
        return cc;
    if (v.contains("thread_safety"))
        cc.thread_safety = parse_ThreadSafety(v["thread_safety"].as_string());
    cc.registration_thread_affine = opt_bool(v, "registration_thread_affine");
    cc.signal_safe = opt_bool(v, "signal_safe");
    cc.callback_origin = opt_string(v, "callback_origin");
    cc.blocking_behavior = opt_string(v, "blocking_behavior");
    cc.suspension_safe = opt_bool(v, "suspension_safe");
    return cc;
}

CallbackContract parse_callback_contract(const JsonValue &v) {
    CallbackContract cc;
    if (v.type != JsonType::Object)
        return cc;
    cc.signature = opt_string(v, "signature");
    if (v.contains("mode"))
        cc.mode = parse_CallbackMode(v["mode"].as_string());
    cc.user_data_parameter = opt_optional_uint(v, "user_data_parameter");
    if (v.contains("lifetime"))
        cc.lifetime = parse_lifetime(v["lifetime"]);
    cc.unregister_function = opt_string(v, "unregister_function");
    cc.reentrant = opt_bool(v, "reentrant");
    cc.may_throw = opt_bool(v, "may_throw");
    return cc;
}

DestructorContract parse_destructor_contract(const JsonValue &v) {
    DestructorContract dc;
    if (v.type != JsonType::Object)
        return dc;
    dc.function_name = opt_string(v, "function_name");
    dc.context_parameter = opt_optional_uint(v, "context_parameter");
    dc.allocator_family = opt_string(v, "allocator_family");
    dc.idempotent = opt_bool(v, "idempotent");
    dc.null_safe = opt_bool(v, "null_safe");
    dc.thread_affine = opt_bool(v, "thread_affine");
    dc.may_fail = opt_bool(v, "may_fail");
    return dc;
}

ConditionalTransfer parse_conditional_transfer(const JsonValue &v) {
    ConditionalTransfer ct;
    if (v.type != JsonType::Object)
        return ct;
    if (v.contains("branch"))
        ct.branch = parse_TransferBranch(v["branch"].as_string());
    ct.from_owner = opt_string(v, "from_owner");
    ct.to_owner = opt_string(v, "to_owner");
    ct.predicate = opt_string(v, "predicate");
    return ct;
}

InvalidationRule parse_invalidation_rule(const JsonValue &v) {
    InvalidationRule ir;
    if (v.type != JsonType::Object)
        return ir;
    if (v.contains("kind"))
        ir.kind = parse_InvalidationKind(v["kind"].as_string());
    ir.description = opt_string(v, "description");
    return ir;
}

ContractProvenance parse_provenance(const JsonValue &v) {
    ContractProvenance p;
    if (v.type != JsonType::Object)
        return p;
    p.header = opt_string(v, "header");
    p.revision = opt_string(v, "revision");
    if (v.contains("source_location"))
        p.source_location = parse_source_span(v["source_location"]);
    p.author = opt_string(v, "author");
    if (v.contains("review_status"))
        p.review_status = parse_ReviewStatus(v["review_status"].as_string());
    p.evidence_method = opt_string(v, "evidence_method");
    p.toolchain_fingerprint = opt_string(v, "toolchain_fingerprint");
    return p;
}

FunctionContract parse_function_contract(const JsonValue &v) {
    FunctionContract fc;
    if (v.type != JsonType::Object)
        return fc;
    fc.c_name = opt_string(v, "c_name");
    if (v.contains("return"))
        fc.return_contract = parse_return_contract(v["return"]);
    if (v.contains("parameters")) {
        const auto &arr = require_array(v["parameters"], "parameters");
        for (std::size_t i = 0; i < arr.size(); ++i)
            fc.parameters.push_back(parse_parameter_contract(arr[i]));
    }
    if (v.contains("errors"))
        fc.error_contract = parse_error_contract(v["errors"]);
    if (v.contains("effects"))
        fc.effects = parse_effect_contract(v["effects"]);
    if (v.contains("concurrency"))
        fc.concurrency = parse_concurrency_contract(v["concurrency"]);
    if (v.contains("callbacks")) {
        const auto &arr = require_array(v["callbacks"], "callbacks");
        for (std::size_t i = 0; i < arr.size(); ++i)
            fc.callbacks.push_back(parse_callback_contract(arr[i]));
    }
    if (v.contains("destructors")) {
        const auto &arr = require_array(v["destructors"], "destructors");
        for (std::size_t i = 0; i < arr.size(); ++i)
            fc.destructors.push_back(parse_destructor_contract(arr[i]));
    }
    if (v.contains("transfers")) {
        const auto &arr = require_array(v["transfers"], "transfers");
        for (std::size_t i = 0; i < arr.size(); ++i)
            fc.transfers.push_back(parse_conditional_transfer(arr[i]));
    }
    if (v.contains("invalidations")) {
        const auto &arr = require_array(v["invalidations"], "invalidations");
        for (std::size_t i = 0; i < arr.size(); ++i)
            fc.invalidations.push_back(parse_invalidation_rule(arr[i]));
    }
    if (v.contains("provenance"))
        fc.provenance = parse_provenance(v["provenance"]);
    return fc;
}

// ── Serialization helpers ────────────────────────────────────────────────

std::string quote(std::string_view s) { return '"' + json_escape(s) + '"'; }
std::string boolean(bool v) { return v ? "true" : "false"; }
std::string span_json(const SourceSpan &s) {
    return "{\"column\":" + std::to_string(s.column) + ",\"end_column\":" +
           std::to_string(s.end_column) + ",\"end_line\":" + std::to_string(s.end_line) +
           ",\"file\":" + quote(s.file) + ",\"line\":" + std::to_string(s.line) + '}';
}
std::string lifetime_json(const LifetimeSpec &l) {
    return "{\"origin\":" + quote(LifetimeOrigin_name(l.origin)) + ",\"parameter_index\":" +
           std::to_string(l.parameter_index) + ",\"region_name\":" + quote(l.region_name) + '}';
}
std::string return_json(const ReturnContract &r) {
    std::string out = "{\"alias_of\":" + quote(r.alias_of) +
                      ",\"allocation_source\":" + quote(r.allocation_source) +
                      ",\"deallocator_family\":" + quote(r.deallocator_family) +
                      ",\"destroy_with\":" + quote(r.destroy_with) +
                      ",\"kind\":" + quote(ReturnKind_name(r.kind)) +
                      ",\"nullable\":" + boolean(r.nullable) +
                      ",\"nullable_means_success\":" + boolean(r.nullable_means_success);
    if (r.lifetime)
        out += ",\"lifetime\":" + lifetime_json(*r.lifetime);
    return out + '}';
}
std::string param_json(const ParameterContract &p) {
    std::string out = "{\"alignment\":";
    if (p.alignment)
        out += std::to_string(*p.alignment);
    else
        out += "null";
    out += ",\"count_parameter\":";
    if (p.count_parameter)
        out += std::to_string(*p.count_parameter);
    else
        out += "null";
    out += ",\"encoding\":" + quote(p.encoding) + ",\"index\":" + std::to_string(p.index) +
           ",\"length_parameter\":";
    if (p.length_parameter)
        out += std::to_string(*p.length_parameter);
    else
        out += "null";
    if (p.lifetime)
        out += ",\"lifetime\":" + lifetime_json(*p.lifetime);
    out += ",\"name\":" + quote(p.name) +
           ",\"nullable\":" + boolean(p.nullable) +
           ",\"ownership\":" + quote(OwnershipKind_name(p.ownership));
    if (p.transfer_branch)
        out += ",\"transfer_branch\":" + quote(TransferBranch_name(*p.transfer_branch));
    out += ",\"transfer_predicate\":" + quote(p.transfer_predicate) + '}';
    return out;
}
std::string error_json(const ErrorContract &e) {
    return "{\"cleanup_required_on_failure\":" + boolean(e.cleanup_required_on_failure) +
           ",\"error_parameter\":" + quote(e.error_parameter) +
           ",\"kind\":" + quote(ErrorKind_name(e.kind)) +
           ",\"out_valid_on_failure\":" + boolean(e.out_valid_on_failure) +
           ",\"sentinel_value\":" + quote(e.sentinel_value) + '}';
}
std::string effects_json(const EffectContract &e) {
    std::string out = "{\"effects\":[";
    bool first = true;
    for (auto eff : e.effects) {
        if (!first)
            out += ',';
        first = false;
        out += quote(EffectKind_name(eff));
    }
    return out + "],\"unknown\":" + boolean(e.unknown) + '}';
}
std::string concurrency_json(const ConcurrencyContract &c) {
    return "{\"blocking_behavior\":" + quote(c.blocking_behavior) +
           ",\"callback_origin\":" + quote(c.callback_origin) +
           ",\"registration_thread_affine\":" + boolean(c.registration_thread_affine) +
           ",\"signal_safe\":" + boolean(c.signal_safe) +
           ",\"suspension_safe\":" + boolean(c.suspension_safe) +
           ",\"thread_safety\":" + quote(ThreadSafety_name(c.thread_safety)) + '}';
}
std::string callback_json(const CallbackContract &c) {
    std::string out = "{\"may_throw\":" + boolean(c.may_throw) +
                      ",\"mode\":" + quote(CallbackMode_name(c.mode)) +
                      ",\"reentrant\":" + boolean(c.reentrant) +
                      ",\"signature\":" + quote(c.signature) +
                      ",\"unregister_function\":" + quote(c.unregister_function);
    if (c.user_data_parameter)
        out += ",\"user_data_parameter\":" + std::to_string(*c.user_data_parameter);
    if (c.lifetime)
        out += ",\"lifetime\":" + lifetime_json(*c.lifetime);
    return out + '}';
}
std::string destructor_json(const DestructorContract &d) {
    std::string out = "{\"allocator_family\":" + quote(d.allocator_family) +
                      ",\"function_name\":" + quote(d.function_name) +
                      ",\"idempotent\":" + boolean(d.idempotent) +
                      ",\"may_fail\":" + boolean(d.may_fail) +
                      ",\"null_safe\":" + boolean(d.null_safe) +
                      ",\"thread_affine\":" + boolean(d.thread_affine);
    if (d.context_parameter)
        out += ",\"context_parameter\":" + std::to_string(*d.context_parameter);
    return out + '}';
}
std::string transfer_json(const ConditionalTransfer &t) {
    return "{\"branch\":" + quote(TransferBranch_name(t.branch)) +
           ",\"from_owner\":" + quote(t.from_owner) +
           ",\"predicate\":" + quote(t.predicate) +
           ",\"to_owner\":" + quote(t.to_owner) + '}';
}
std::string invalidation_json(const InvalidationRule &r) {
    return "{\"description\":" + quote(r.description) +
           ",\"kind\":" + quote(InvalidationKind_name(r.kind)) + '}';
}
std::string provenance_json(const ContractProvenance &p) {
    return "{\"author\":" + quote(p.author) + ",\"evidence_method\":" + quote(p.evidence_method) +
           ",\"header\":" + quote(p.header) + ",\"revision\":" + quote(p.revision) +
           ",\"review_status\":" + quote(ReviewStatus_name(p.review_status)) +
           ",\"source_location\":" + span_json(p.source_location) +
           ",\"toolchain_fingerprint\":" + quote(p.toolchain_fingerprint) + '}';
}
std::string function_json(const FunctionContract &f) {
    std::string out = "{\"c_name\":" + quote(f.c_name);
    if (!f.callbacks.empty()) {
        out += ",\"callbacks\":[";
        for (std::size_t i = 0; i < f.callbacks.size(); ++i) {
            if (i)
                out += ',';
            out += callback_json(f.callbacks[i]);
        }
        out += ']';
    }
    out += ",\"concurrency\":" + concurrency_json(f.concurrency);
    if (!f.destructors.empty()) {
        out += ",\"destructors\":[";
        for (std::size_t i = 0; i < f.destructors.size(); ++i) {
            if (i)
                out += ',';
            out += destructor_json(f.destructors[i]);
        }
        out += ']';
    }
    out += ",\"effects\":" + effects_json(f.effects) +
           ",\"errors\":" + error_json(f.error_contract);
    if (!f.invalidations.empty()) {
        out += ",\"invalidations\":[";
        for (std::size_t i = 0; i < f.invalidations.size(); ++i) {
            if (i)
                out += ',';
            out += invalidation_json(f.invalidations[i]);
        }
        out += ']';
    }
    if (!f.parameters.empty()) {
        out += ",\"parameters\":[";
        for (std::size_t i = 0; i < f.parameters.size(); ++i) {
            if (i)
                out += ',';
            out += param_json(f.parameters[i]);
        }
        out += ']';
    }
    out += ",\"provenance\":" + provenance_json(f.provenance) +
           ",\"return\":" + return_json(f.return_contract);
    if (!f.transfers.empty()) {
        out += ",\"transfers\":[";
        for (std::size_t i = 0; i < f.transfers.size(); ++i) {
            if (i)
                out += ',';
            out += transfer_json(f.transfers[i]);
        }
        out += ']';
    }
    return out + '}';
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

ContractBundle parse_contract_sidecar(std::string_view json) {
    auto root = detail::parse_json(json);
    if (root.type != JsonType::Object)
        throw std::invalid_argument("contract sidecar must be a JSON object");

    ContractBundle bundle;
    if (root.contains("schema_version"))
        bundle.schema_version = static_cast<unsigned>(root["schema_version"].as_int());
    bundle.library = opt_string(root, "library");
    if (root.contains("headers")) {
        const auto &arr = require_array(root["headers"], "headers");
        for (std::size_t i = 0; i < arr.size(); ++i)
            bundle.headers.push_back(arr[i].as_string());
    }
    if (root.contains("state"))
        bundle.state = parse_ContractState(root["state"].as_string());
    if (root.contains("functions")) {
        const auto &arr = require_array(root["functions"], "functions");
        for (std::size_t i = 0; i < arr.size(); ++i)
            bundle.functions.push_back(parse_function_contract(arr[i]));
    }
    if (root.contains("bundle_provenance"))
        bundle.bundle_provenance = parse_provenance(root["bundle_provenance"]);
    return bundle;
}

std::string serialize_contract(const ContractBundle &bundle) {
    std::string out = "{\"bundle_provenance\":" + provenance_json(bundle.bundle_provenance);
    if (!bundle.headers.empty()) {
        out += ",\"headers\":[";
        for (std::size_t i = 0; i < bundle.headers.size(); ++i) {
            if (i)
                out += ',';
            out += quote(bundle.headers[i]);
        }
        out += ']';
    }
    out += ",\"library\":" + quote(bundle.library);
    out += ",\"schema_version\":" + std::to_string(bundle.schema_version);
    out += ",\"state\":" + quote(ContractState_name(bundle.state));
    if (!bundle.functions.empty()) {
        out += ",\"functions\":[";
        for (std::size_t i = 0; i < bundle.functions.size(); ++i) {
            if (i)
                out += ',';
            out += function_json(bundle.functions[i]);
        }
        out += ']';
    }
    return out + "}\n";
}

Diagnostics validate_contract(const ContractBundle &bundle,
                              const cdecl::TranslationUnit &unit) {
    Diagnostics diags;
    auto reject = [&](std::string id, std::string message, SourceSpan span = {}) {
        Diagnostic d;
        d.id = std::move(id);
        d.message = std::move(message);
        d.severity = Severity::Error;
        d.primary = std::move(span);
        diags.push_back(std::move(d));
    };

    if (bundle.schema_version != contract_schema_version)
        reject("EXOML-CONTRACT-VERSION",
               "unsupported contract schema version " + std::to_string(bundle.schema_version) +
                   "; expected " + std::to_string(contract_schema_version));

    std::map<std::string, const cdecl::Declaration *> decl_index;
    for (const auto &d : unit.declarations)
        if (d.kind == cdecl::DeclKind::Function)
            decl_index[d.name] = &d;

    std::set<std::string> function_names;
    for (const auto &fn : bundle.functions) {
        if (!function_names.insert(fn.c_name).second)
            reject("EXOML-CONTRACT-DUPLICATE", "duplicate function contract: " + fn.c_name);

        auto it = decl_index.find(fn.c_name);
        if (it == decl_index.end()) {
            reject("EXOML-CONTRACT-DRIFT",
                   "contract references unknown declaration: " + fn.c_name,
                   fn.provenance.source_location);
            continue;
        }
        const auto &decl = *it->second;

        if (fn.parameters.size() != decl.parameters.size())
            reject("EXOML-CONTRACT-DRIFT",
                   "parameter count mismatch for " + fn.c_name + ": contract has " +
                       std::to_string(fn.parameters.size()) + ", declaration has " +
                       std::to_string(decl.parameters.size()),
                   fn.provenance.source_location);

        // Check for contradictory ownership within parameters
        std::set<std::uint32_t> consumed, borrowed;
        for (const auto &p : fn.parameters) {
            if (p.ownership == OwnershipKind::Consumed)
                consumed.insert(p.index);
            if (p.ownership == OwnershipKind::BorrowedImmutable ||
                p.ownership == OwnershipKind::BorrowedMutable)
                borrowed.insert(p.index);
        }
        for (auto idx : consumed)
            if (borrowed.count(idx))
                reject("EXOML-CONTRACT-CONTRADICT",
                       "parameter " + std::to_string(idx) + " in " + fn.c_name +
                           " is both consumed and borrowed");

        // Return borrow without lifetime source
        if (fn.return_contract.kind == ReturnKind::Borrow && !fn.return_contract.lifetime)
            reject("EXOML-CONTRACT-LIFETIME",
                   "returned borrow in " + fn.c_name + " has no lifetime source");
        if (fn.return_contract.kind == ReturnKind::Borrow && fn.return_contract.lifetime &&
            fn.return_contract.lifetime->origin == LifetimeOrigin::Parameter) {
            auto pi = fn.return_contract.lifetime->parameter_index;
            bool found = false;
            for (const auto &p : fn.parameters)
                if (p.index == pi) {
                    found = true;
                    break;
                }
            if (!found)
                reject("EXOML-CONTRACT-LIFETIME",
                       "returned borrow in " + fn.c_name +
                           " references nonexistent parameter index " + std::to_string(pi));
        }

        // Owned return without destructor
        if (fn.return_contract.kind == ReturnKind::Owned &&
            fn.return_contract.destroy_with.empty())
            reject("EXOML-CONTRACT-ALLOCATOR",
                   "owned return in " + fn.c_name + " has no destroy_with function");

        // Pointer+length association
        for (const auto &p : fn.parameters) {
            if (p.length_parameter) {
                bool found = false;
                for (const auto &q : fn.parameters)
                    if (q.index == *p.length_parameter) {
                        found = true;
                        break;
                    }
                if (!found)
                    reject("EXOML-CONTRACT-LENGTH",
                           "parameter " + std::to_string(p.index) + " in " + fn.c_name +
                               " references nonexistent length parameter " +
                               std::to_string(*p.length_parameter));
            }
            if (p.count_parameter) {
                bool found = false;
                for (const auto &q : fn.parameters)
                    if (q.index == *p.count_parameter) {
                        found = true;
                        break;
                    }
                if (!found)
                    reject("EXOML-CONTRACT-LENGTH",
                           "parameter " + std::to_string(p.index) + " in " + fn.c_name +
                               " references nonexistent count parameter " +
                               std::to_string(*p.count_parameter));
            }
        }

        // Retained callback without unregister
        for (const auto &cb : fn.callbacks)
            if (cb.mode == CallbackMode::Retained && cb.unregister_function.empty())
                reject("EXOML-CONTRACT-CALLBACK",
                       "retained callback in " + fn.c_name +
                           " has no unregister function");

        // Destructor allocator consistency
        std::set<std::string> allocator_families;
        for (const auto &d : fn.destructors)
            if (!d.allocator_family.empty())
                allocator_families.insert(d.allocator_family);
        if (!fn.return_contract.deallocator_family.empty() &&
            !fn.return_contract.destroy_with.empty()) {
            bool match = false;
            for (const auto &d : fn.destructors)
                if (d.function_name == fn.return_contract.destroy_with) {
                    if (d.allocator_family == fn.return_contract.deallocator_family)
                        match = true;
                    break;
                }
            if (!match && !allocator_families.empty())
                reject("EXOML-CONTRACT-ALLOCATOR",
                       "destructor allocator mismatch in " + fn.c_name);
        }

        // Duplicate destructors for same function
        std::set<std::string> destructor_names;
        for (const auto &d : fn.destructors)
            if (!destructor_names.insert(d.function_name).second)
                reject("EXOML-CONTRACT-DUPLICATE",
                       "duplicate destructor in " + fn.c_name + ": " + d.function_name);
    }

    return diags;
}

OwnershipProjection project_to_ownership(const FunctionContract &contract,
                                         std::size_t parameter_count) {
    OwnershipProjection proj;
    (void)parameter_count;
    for (const auto &p : contract.parameters) {
        switch (p.ownership) {
        case OwnershipKind::Consumed:
        case OwnershipKind::Transferred:
            proj.ownership.consumed_arguments.insert(p.index);
            break;
        case OwnershipKind::BorrowedImmutable:
        case OwnershipKind::BorrowedMutable:
            proj.ownership.borrowed_arguments.insert(p.index);
            break;
        default:
            break;
        }
    }
    if (contract.return_contract.kind == ReturnKind::Borrow)
        proj.ownership.returned_borrow = true;
    for (auto eff : contract.effects.effects) {
        switch (eff) {
        case EffectKind::Read:
            proj.ownership.effects.effects.insert(Effect::Read);
            break;
        case EffectKind::Write:
            proj.ownership.effects.effects.insert(Effect::Write);
            break;
        case EffectKind::Allocate:
            proj.ownership.effects.effects.insert(Effect::Allocate);
            break;
        case EffectKind::Free:
            proj.ownership.effects.effects.insert(Effect::Free);
            break;
        case EffectKind::Escape:
            proj.ownership.effects.effects.insert(Effect::Escape);
            break;
        case EffectKind::Thread:
            proj.ownership.effects.effects.insert(Effect::ThreadSpawn);
            break;
        case EffectKind::Suspend:
            proj.ownership.effects.effects.insert(Effect::Suspend);
            break;
        case EffectKind::ForeignCall:
            proj.ownership.effects.effects.insert(Effect::ForeignCall);
            break;
        case EffectKind::IO:
            break;
        }
    }
    proj.ownership.effects.unknown = contract.effects.unknown;
    if (contract.return_contract.kind == ReturnKind::Owned ||
        contract.return_contract.nullable)
        proj.ownership.may_escape = true;
    return proj;
}

bool is_safe_for_export(const ContractBundle &bundle) {
    if (bundle.functions.empty())
        return false;
    for (const auto &fn : bundle.functions) {
        if (fn.provenance.review_status != ReviewStatus::Reviewed &&
            fn.provenance.review_status != ReviewStatus::Verified)
            return false;
    }
    return bundle.state == ContractState::Reviewed ||
           bundle.state == ContractState::UsageChecked;
}

} // namespace moonstone::exoml::contract
