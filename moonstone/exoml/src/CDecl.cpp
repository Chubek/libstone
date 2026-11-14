#include <exoml/CDecl.hpp>
#include "JsonReader.hpp"
#include <iterator>
#include <stdexcept>

namespace moonstone::exoml::cdecl {
namespace {
std::string quote(std::string_view s) { return '"' + json_escape(s) + '"'; }
template <class T> std::string number(const std::optional<T> &v) {
    return v ? std::to_string(*v) : "null";
}
std::string boolean(bool v) { return v ? "true" : "false"; }
std::string span(const SourceSpan &s) {
    return "{\"file\":" + quote(s.file) + ",\"line\":" + std::to_string(s.line) +
           ",\"column\":" + std::to_string(s.column) +
           ",\"end_line\":" + std::to_string(s.end_line) +
           ",\"end_column\":" + std::to_string(s.end_column) + '}';
}
template <class T, class F> std::string array(const T &items, F emit) {
    std::string out = "[";
    bool first = true;
    for (const auto &item : items) {
        if (!first)
            out += ',';
        first = false;
        out += emit(item);
    }
    return out + ']';
}
std::string strings(const std::vector<std::string> &s) { return array(s, quote); }
std::string preprocessing(const Preprocessing &p) {
    return "{\"coverage\":" + quote(p.coverage) +
           ",\"final_snapshot_complete\":" + boolean(p.final_snapshot_complete) +
           ",\"definitions\":" +
           array(p.definitions,
                 [](const auto &d) {
                     return "{\"id\":" + quote(d.id) + ",\"name\":" + quote(d.name) +
                            ",\"tokens\":" + strings(d.tokens) + ",\"source\":" + span(d.source) +
                            ",\"function_like\":" + boolean(d.function_like) +
                            ",\"builtin\":" + boolean(d.builtin) + '}';
                 }) +
           ",\"expansions\":" +
           array(p.expansions,
                 [](const auto &e) {
                     return "{\"name\":" + quote(e.name) +
                            ",\"definition_id\":" + quote(e.definition_id) +
                            ",\"unknown_reason\":" + quote(e.unknown_reason) +
                            ",\"source\":" + span(e.source) + '}';
                 }) +
           ",\"final_macros\":" +
           array(p.final_macros,
                 [](const auto &s) {
                     return "{\"name\":" + quote(s.name) +
                            ",\"definition_id\":" + quote(s.definition_id) +
                            ",\"unknown_reason\":" + quote(s.unknown_reason) +
                            ",\"defined\":" + (s.defined ? boolean(*s.defined) : "null") + '}';
                 }) +
           ",\"includes\":" +
           array(p.includes,
                 [](const auto &e) {
                     return "{\"included\":" + quote(e.included) + ",\"source\":" + span(e.source) +
                            '}';
                 }) +
           ",\"skipped_ranges\":" + array(p.skipped_ranges, span) + '}';
}
} // namespace
std::string describe_translation_unit(const TranslationUnit &u) {
    static constexpr const char *type_names[] = {"unknown", "void",  "integer",   "floating",
                                                 "pointer", "array", "function",  "typedef",
                                                 "record",  "enum",  "elaborated"};
    static constexpr const char *decl_names[] = {"unsupported", "function", "typedef", "record",
                                                 "enum",        "variable", "macro"};
    auto type_name = [&](TypeKind k) {
        auto i = static_cast<unsigned>(k);
        return quote(i < std::size(type_names) ? type_names[i] : "unknown");
    };
    return "{\"schema_version\":" + std::to_string(u.version) +
           ",\"extractor_version\":" + quote(extractor_version) +
           ",\"state\":\"discovered\","
           "\"safe_export\":false,\"unknown_effects\":true,\"complete\":" +
           boolean(u.complete) + ",\"header\":" + quote(u.header) +
           ",\"revision\":" + quote(u.revision) + ",\"fingerprint\":" + quote(u.fingerprint) +
           ",\"target\":{\"compiler\":" + quote(u.target.compiler) +
           ",\"triple\":" + quote(u.target.triple) +
           ",\"pointer_bits\":" + std::to_string(u.target.pointer_bits) + "},\"compilation\":" +
           (u.compilation
                ? "{\"database\":" + quote(u.compilation->database) + ",\"directory\":" +
                      quote(u.compilation->directory) + ",\"file\":" + quote(u.compilation->file) +
                      ",\"driver\":" + quote(u.compilation->driver) +
                      ",\"original_arguments\":" + strings(u.compilation->original_arguments) +
                      ",\"ignored_arguments\":" + strings(u.compilation->ignored_arguments) + '}'
                : "null") +
           ",\"preprocessing\":" + preprocessing(u.preprocessing) +
           ",\"arguments\":" + strings(u.arguments) + ",\"inputs\":" +
           array(u.inputs,
                 [](const auto &i) {
                     return "{\"path\":" + quote(i.path) + ",\"sha256\":" + quote(i.sha256) + '}';
                 }) +
           ",\"types\":" +
           array(u.types,
                 [&](const auto &entry) {
                     const auto &t = entry.second;
                     return "{\"id\":" + quote(t.id) +
                            ",\"canonical_id\":" + quote(t.canonical_id) +
                            ",\"spelling\":" + quote(t.spelling) +
                            ",\"canonical_spelling\":" + quote(t.canonical_spelling) +
                            ",\"declaration_id\":" + quote(t.declaration_id) +
                            ",\"kind\":" + type_name(t.kind) +
                            ",\"const\":" + boolean(t.qualifiers.is_const) +
                            ",\"volatile\":" + boolean(t.qualifiers.is_volatile) +
                            ",\"restrict\":" + boolean(t.qualifiers.is_restrict) +
                            ",\"size\":" + number(t.size) +
                            ",\"alignment\":" + number(t.alignment) +
                            ",\"array_count\":" + number(t.array_count) +
                            ",\"signed\":" + (t.is_signed ? boolean(*t.is_signed) : "null") +
                            ",\"element\":" + quote(t.element) + ",\"result\":" + quote(t.result) +
                            ",\"arguments\":" + strings(t.arguments) +
                            ",\"variadic\":" + boolean(t.variadic) +
                            ",\"prototype\":" + boolean(t.prototype) +
                            ",\"union\":" + boolean(t.is_union) +
                            ",\"calling_convention\":" + std::to_string(t.calling_convention) +
                            ",\"attributes\":" + strings(t.attributes) +
                            ",\"unsupported_reason\":" + quote(t.unsupported_reason) +
                            ",\"fields\":" +
                            array(t.fields,
                                  [](const auto &f) {
                                      return "{\"name\":" + quote(f.name) +
                                             ",\"type\":" + quote(f.type) +
                                             ",\"source\":" + span(f.source) +
                                             ",\"offset_bits\":" + number(f.offset_bits) +
                                             ",\"bit_width\":" + number(f.bit_width) + '}';
                                  }) +
                            ",\"enumerators\":" +
                            array(t.enumerators,
                                  [](const auto &e) {
                                      return "{\"name\":" + quote(e.name) +
                                             ",\"value\":" + quote(e.value) + '}';
                                  }) +
                            '}';
                 }) +
           ",\"declarations\":" +
           array(u.declarations,
                 [&](const auto &d) {
                     auto k = static_cast<unsigned>(d.kind);
                     return "{\"id\":" + quote(d.id) + ",\"name\":" + quote(d.name) + ",\"kind\":" +
                            quote(k < std::size(decl_names) ? decl_names[k] : "unsupported") +
                            ",\"type\":" + quote(d.type) + ",\"source\":" + span(d.source) +
                            ",\"definition\":" + boolean(d.definition) +
                            ",\"inline\":" + boolean(d.is_inline) +
                            ",\"function_like_macro\":" + boolean(d.function_like_macro) +
                            ",\"linkage\":" + quote(d.linkage) + ",\"tokens\":" + quote(d.tokens) +
                            ",\"unsupported_reason\":" + quote(d.unsupported_reason) +
                            ",\"parameters\":" +
                            array(d.parameters,
                                  [](const auto &p) {
                                      return "{\"name\":" + quote(p.name) +
                                             ",\"type\":" + quote(p.type) +
                                             ",\"source\":" + span(p.source) +
                                             ",\"tokens\":" + quote(p.tokens) + '}';
                                  }) +
                            '}';
                 }) +
           ",\"diagnostics\":" +
           array(u.diagnostics,
                 [](const auto &d) {
                     return "{\"id\":" + quote(d.id) + ",\"severity\":" + quote(name(d.severity)) +
                            ",\"message\":" + quote(d.message) + ",\"source\":" + span(d.primary) +
                            ",\"related\":" + array(d.related, span) + '}';
                 }) +
            "}\n";
}

TranslationUnit parse_translation_unit_json(std::string_view json) {
    namespace detail = moonstone::exoml::contract::detail;
    auto root = detail::parse_json(json);
    if (root.type != detail::JsonType::Object)
        throw std::invalid_argument("expected JSON object for translation unit");

    TranslationUnit unit;
    if (root.contains("schema_version"))
        unit.version = static_cast<unsigned>(root["schema_version"].as_int());
    if (root.contains("header"))
        unit.header = root["header"].as_string();
    if (root.contains("revision"))
        unit.revision = root["revision"].as_string();
    if (root.contains("fingerprint"))
        unit.fingerprint = root["fingerprint"].as_string();
    if (root.contains("complete"))
        unit.complete = root["complete"].as_bool();
    if (root.contains("target") && root["target"].type == detail::JsonType::Object) {
        const auto &t = root["target"];
        if (t.contains("compiler"))
            unit.target.compiler = t["compiler"].as_string();
        if (t.contains("triple"))
            unit.target.triple = t["triple"].as_string();
        if (t.contains("pointer_bits"))
            unit.target.pointer_bits = static_cast<unsigned>(t["pointer_bits"].as_int());
    }

    static constexpr const char *decl_names[] = {"unsupported", "function", "typedef", "record",
                                                  "enum", "variable", "macro"};
    auto parse_decl_kind = [](std::string_view s) -> DeclKind {
        for (std::size_t i = 0; i < std::size(decl_names); ++i)
            if (decl_names[i] == s)
                return static_cast<DeclKind>(i);
        return DeclKind::Unsupported;
    };

    if (root.contains("declarations") && root["declarations"].type == detail::JsonType::Array) {
        const auto &arr = root["declarations"];
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const auto &obj = arr[i];
            Declaration d;
            if (obj.contains("id"))
                d.id = obj["id"].as_string();
            if (obj.contains("name"))
                d.name = obj["name"].as_string();
            if (obj.contains("type"))
                d.type = obj["type"].as_string();
            if (obj.contains("kind"))
                d.kind = parse_decl_kind(obj["kind"].as_string());
            if (obj.contains("source") && obj["source"].type == detail::JsonType::Object) {
                const auto &s = obj["source"];
                if (s.contains("file"))
                    d.source.file = s["file"].as_string();
                if (s.contains("line"))
                    d.source.line = static_cast<std::uint32_t>(s["line"].as_int());
                if (s.contains("column"))
                    d.source.column = static_cast<std::uint32_t>(s["column"].as_int());
                if (s.contains("end_line"))
                    d.source.end_line = static_cast<std::uint32_t>(s["end_line"].as_int());
                if (s.contains("end_column"))
                    d.source.end_column = static_cast<std::uint32_t>(s["end_column"].as_int());
            }
            if (obj.contains("definition"))
                d.definition = obj["definition"].as_bool();
            if (obj.contains("inline"))
                d.is_inline = obj["inline"].as_bool();
            if (obj.contains("function_like_macro"))
                d.function_like_macro = obj["function_like_macro"].as_bool();
            if (obj.contains("linkage"))
                d.linkage = obj["linkage"].as_string();
            if (obj.contains("parameters") &&
                obj["parameters"].type == detail::JsonType::Array) {
                const auto &params = obj["parameters"];
                for (std::size_t j = 0; j < params.size(); ++j) {
                    const auto &p = params[j];
                    Parameter param;
                    if (p.contains("name"))
                        param.name = p["name"].as_string();
                    if (p.contains("type"))
                        param.type = p["type"].as_string();
                    d.parameters.push_back(std::move(param));
                }
            }
            unit.declarations.push_back(std::move(d));
        }
    }
    return unit;
}

} // namespace moonstone::exoml::cdecl
