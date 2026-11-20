#ifndef ARCHON_IRGEN_EMBEDDING_HPP
#define ARCHON_IRGEN_EMBEDDING_HPP

#include <archon/Archon.hpp>

namespace archon::irgen {

// These are host-side generators, not the standalone generated C APIs.
using Artifacts = std::map<std::string, std::string>;

Value get_export(std::string_view name);
Value dispatch(std::string_view name, const List& args = {}, const Object& kwargs = {});
std::vector<std::string> exports();

inline std::vector<std::string> available_representations() {
    return detail::strings(irgen::dispatch("available_representations"));
}
inline Value get_representation(std::string_view name) {
    return irgen::dispatch("get_representation", {name});
}
inline std::vector<std::string> pipeline_stages() {
    return detail::strings(irgen::dispatch("pipeline_stages"));
}
inline Value build_context(std::string_view ir, const List& databases, const Object& options = {}) {
    return irgen::dispatch("build_context", {ir, databases}, {{"options", options}});
}

namespace detail_irgen {
inline Artifacts artifacts(const Value& value) {
    Artifacts result;
    for (const auto& entry : value.as_object())
        result.emplace(entry.first, entry.second.as_string());
    return result;
}
}

inline Artifacts emit_stage(std::string_view stage, const Value& context) {
    return detail_irgen::artifacts(irgen::dispatch("emit_stage", {stage, context}));
}
inline Artifacts generate_ir(std::string_view ir, const List& databases,
                             const Object& options = {}, bool run_stages = true) {
    return detail_irgen::artifacts(irgen::dispatch(
        "generate_ir", {ir, databases}, {{"options", options}, {"run_stages", run_stages}}));
}
// List all protected sources (including scripts/banks/tools named by path
// in generation options) in input_paths, especially when using force.
inline std::string write_generated_directory(const Artifacts& artifacts,
                                             std::string_view destination,
                                             const List& input_paths = {}, bool force = false) {
    Object files;
    for (const auto& entry : artifacts)
        files.emplace(entry.first, entry.second);
    return irgen::dispatch("write_generated_directory", {files, destination},
                           {{"input_paths", input_paths}, {"force", force}}).as_path();
}
inline std::vector<std::string> standard_spec_entry_points(const Value& root = {}) {
    std::vector<std::string> result;
    for (const auto& item : irgen::dispatch("standard_spec_entry_points", {root}).as_list())
        result.push_back(item.as_path());
    return result;
}
inline List compile_standard_architectures(const Value& root = {}) {
    return irgen::dispatch("compile_standard_architectures", {root}).as_list();
}

// These existing RV64IM LP64 bridges execute caller-selected trusted tools.
inline void assemble_object(std::string_view backend, std::string_view assembler,
                            std::string_view flavor, std::string_view source,
                            std::string_view output) {
    irgen::dispatch("assemble_object", {backend, assembler, flavor, source, output});
}
inline void link_executable(std::string_view linker, const List& objects,
                            std::string_view entry, std::string_view output) {
    irgen::dispatch("link_executable", {linker, objects, entry, output});
}

} // namespace archon::irgen

#endif
