// Legacy spec compiler adapters. The assembler metadata mapping is shared
// with the built-in DSymspec profiles and does not depend on the compiler.
#include "domsymascc/records.hpp"
#include "../../decoder/spec_records.hpp"
#ifndef SANDSTONE_RECORDS_ONLY
#include "domsymascc/domsymascc.hpp"
#endif

namespace domsymas::cc {

Result compile_assembler_records(const std::string &text, Builder &builder) {
    auto res = detail::build_spec_records(detail::json::parse(text), builder);
    return {res.ok, std::move(res.error), {}};
}

#ifndef SANDSTONE_RECORDS_ONLY
namespace {
using detail::Value;

// Keep the original Boost.JSON document for lossless serialization.
Value assembler_value(const symspec::Json &input) {
    if (input.is_null()) return Value::null();
    if (input.is_bool()) return Value::boolean_value(input.as_bool());
    if (input.is_int64()) return Value::integer_value(input.as_int64());
    if (input.is_uint64()) {
        if (input.as_uint64() > uint64_t(INT64_MAX))
            throw std::runtime_error("assembler integer exceeds signed 64-bit range");
        return Value::integer_value(static_cast<int64_t>(input.as_uint64()));
    }
    if (input.is_double()) {
        if (auto n = symspec::integer(&input)) return Value::integer_value(*n);
        throw std::runtime_error("assembler fields require integral numbers");
    }
    if (input.is_string()) return Value::string_value(std::string(input.as_string()));
    if (input.is_array()) {
        auto result = Value::array_value();
        for (const auto &item : input.as_array()) result.array->push_back(assembler_value(item));
        return result;
    }
    auto result = Value::object_value();
    for (const auto &item : input.as_object())
        result.object->emplace_back(std::string(item.key()), assembler_value(item.value()));
    return result;
}
}

Result compile_document(const std::string &document_json, Builder &builder) {
    auto spec = symspec::Spec::from_json(document_json);
    if (!spec) return {false, "spec JSON: " + spec.error().message, {}};
    return compile_spec(*spec, builder);
}

Result compile_file(const symspec::Path &path, Builder &builder,
                    const symspec::LoadOptions &options) {
    auto spec = symspec::Spec::load(path, options);
    if (!spec) return {false, spec.error().message, {}};
    return compile_spec(*spec, builder);
}

Result compile_spec(const symspec::Spec &spec, Builder &builder) {
    if (spec.backend_only())
        return {false, "backend-only specifications have no assembler records; use backend export", {}};
    Value doc = Value::object_value();
    try {
        for (auto key : {"schema", "targets", "features", "registers", "syntaxes",
                         "relocs", "sections", "linkages", "mnemonics", "encoders",
                         "forms", "debug", "models", "units", "isels", "scheds",
                         "tunes", "fusions"}) {
            if (const auto *value = symspec::field(spec.json(), key))
                doc.object->emplace_back(key, assembler_value(*value));
        }
    } catch (const std::exception &e) {
        return {false, std::string("spec JSON parse failed: ") + e.what(), {}};
    }
    auto res = detail::build_spec_records(doc, builder);
    return {res.ok, std::move(res.error), res.ok ? *spec.serialize() : std::string()};
}

Result compile_document_to_bank(const std::string &document_json, Databank &out) {
    Builder builder;
    Result res = compile_document(document_json, builder);
    if (!res.ok) return res;
    Error e = builder.build(out);
    if (!e.ok()) {
        res.ok = false;
        res.error = "builder failed: " + e.message;
    }
    return res;
}
#endif
} // namespace domsymas::cc
