#include "asdl/schema.hpp"
#include "lexer.hpp"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

namespace asdl {
namespace {
namespace fs = std::filesystem;

std::string read_file(const fs::path &path) {
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "cannot open " + path.string());
    std::string contents;
    try {
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure &error) {
        throw std::system_error(error.code(), "cannot read " + path.string());
    }
    if (input.bad())
        throw std::system_error(EIO, std::generic_category(), "cannot read " + path.string());
    return contents;
}

fs::path canonical_path(const fs::path &path) {
    std::error_code error;
    auto result = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}

std::string leaf(std::string_view path) {
    auto split = path.find_last_of('.');
    return std::string(path.substr(split == path.npos ? 0 : split + 1));
}

std::string local_name(const ImportName &name) {
    return name.alias.empty() ? name.name : name.alias;
}

std::string alias_name(const Import &entry) {
    return entry.alias.empty() ? leaf(entry.module) : entry.alias;
}

std::string encoded_module(std::string_view name) {
    std::string result;
    while (!name.empty()) {
        auto split = name.find('.');
        auto component = name.substr(0, split);
        result += std::to_string(component.size()) + "_" + std::string(component) + "_";
        if (split == name.npos) break;
        name.remove_prefix(split + 1);
    }
    // End the module encoding explicitly: otherwise a type name resembling an
    // encoded component could collide with a type from a nested module.
    return result + "0_";
}

struct Document {
    Schema schema;
    std::string module_key;
    bool active = true;
    std::map<std::string, std::string> local, selected;
    std::map<std::string, Document *> namespaces;
    std::set<std::string> lua_names, lua_namespaces, hooks;
    std::map<std::string, Generic> generics;

    Document(Schema input, std::string module)
        : schema(std::move(input)), module_key(std::move(module)) {
        for (const auto &definition : schema.definitions) {
            if (primitives().count(definition.name))
                throw Diagnostic(definition.span, "cannot redefine primitive '" + definition.name + "'");
            auto key = module_key.empty() ? definition.name :
                                           "asdl_import_" + module_key + definition.name;
            if (!local.emplace(definition.name, std::move(key)).second)
                throw Diagnostic(definition.span, "duplicate type '" + definition.name + "'");
        }
    }

    std::string visible_type(const std::string &name, const SourceSpan &span) const {
        if (primitives().count(name)) return name;
        auto split = name.find_last_of('.');
        if (split != name.npos) {
            auto module = name.substr(0, split);
            auto found = namespaces.find(module);
            if (found == namespaces.end())
                throw Diagnostic(span, "unknown schema import alias '" + module + "'");
            auto member = found->second->local.find(name.substr(split + 1));
            if (member != found->second->local.end()) return member->second;
        } else {
            if (auto found = local.find(name); found != local.end()) return found->second;
            if (auto found = selected.find(name); found != selected.end()) return found->second;
        }
        throw Diagnostic(span, "undefined type '" + name + "'");
    }

    std::string resolve_type(const std::string &name, const SourceSpan &span) {
        auto found = schema.generics.find(name);
        if (found == schema.generics.end()) return visible_type(name, span);
        auto generic = found->second;
        auto key = generic.kind + "[";
        for (std::size_t i = 0; i < generic.args.size(); ++i) {
            auto &arg = generic.args[i];
            if (i) key += ",";
            if (generic.kind != "array" || i + 1 != generic.args.size())
                arg = resolve_type(arg, span);
            key += arg;
        }
        key += "]";
        generics.emplace(key, std::move(generic));
        return key;
    }

    void resolve_fields(std::vector<Field> &fields) {
        for (auto &field : fields) {
            field.type = resolve_type(field.type, field.span);
            if (!field.executable_refinement) continue;
            auto split = field.refinement.find_last_of('.');
            if (split == field.refinement.npos ? lua_names.count(field.refinement) :
                lua_namespaces.count(field.refinement.substr(0, split)))
                continue;
            throw Diagnostic(field.refinement_span,
                             "unimported executable refinement '" + field.refinement + "'");
        }
    }

    void rewrite() {
        for (auto &definition : schema.definitions) {
            definition.name = local.at(definition.name);
            resolve_fields(definition.fields);
            resolve_fields(definition.attributes);
            for (auto &ctor : definition.constructors) {
                if (!module_key.empty()) ctor.name = "ASDLImport_" + module_key + ctor.name;
                resolve_fields(ctor.fields);
            }
        }
        if (!schema.unit.empty()) {
            auto name = visible_type(schema.unit, schema.unit_span);
            if (primitives().count(name))
                throw Diagnostic(schema.unit_span, "unit must name an ASDL definition");
            schema.unit = std::move(name);
        }
        for (auto &hook : schema.logistics) {
            auto type = visible_type(hook.type, hook.span);
            if (primitives().count(type))
                throw Diagnostic(hook.span, "logistics target must name an ASDL definition");
            if (!hooks.insert(type).second)
                throw Diagnostic(hook.span, "duplicate logistics for '" + hook.type + "'");
            hook.type = std::move(type);
        }
        schema.generics = std::move(generics);
    }

    void add_lua_import(const Import &entry) {
        auto add = [&](const std::string &name, const SourceSpan &span, bool ns) {
            if (!lua_names.insert(name).second)
                throw Diagnostic(span, "duplicate Lua import binding '" + name + "'");
            if (ns) lua_namespaces.insert(name);
        };
        if (entry.names.empty()) add(alias_name(entry), entry.span, true);
        else for (const auto &name : entry.names) add(local_name(name), name.span, false);
    }
};

class Resolver {
public:
    explicit Resolver(const std::vector<fs::path> &roots) : roots(roots) {}

    Schema run(const Schema &schema) {
        Document root(schema, {});
        auto root_path = canonical_path(root.schema.file);
        files.emplace(root_path, &root);
        walk(root, root_path);
        auto result = std::move(root.schema);
        for (auto &imported : documents) {
            for (auto &definition : imported->schema.definitions)
                result.definitions.push_back(std::move(definition));
            result.generics.merge(imported->schema.generics);
            for (auto &entry : imported->schema.imports)
                result.imports.push_back(std::move(entry));
            for (auto &hook : imported->schema.logistics)
                result.logistics.push_back(std::move(hook));
        }
        std::set<std::string> hooks;
        for (const auto &hook : result.logistics) {
            if (!hooks.insert(hook.type).second)
                throw Diagnostic(hook.span, "duplicate logistics for '" + hook.type + "'");
        }
        result.resolved = true;
        return result;
    }

private:
    const std::vector<fs::path> &roots;
    std::map<fs::path, Document *> files;
    std::map<std::string, fs::path> module_paths;
    std::vector<std::unique_ptr<Document>> documents;
    std::vector<fs::path> stack;

    fs::path find_file(const Import &entry, const fs::path &from) {
        auto rel = entry.module;
        for (auto &c : rel) if (c == '.') c = '/';
        rel += ".asdl";
        std::vector<fs::path> dirs;
        dirs.reserve(roots.size() + 1);
        dirs.push_back(from.parent_path());
        dirs.insert(dirs.end(), roots.begin(), roots.end());
        for (const auto &dir : dirs) {
            auto candidate = dir / rel;
            std::error_code error;
            if (fs::is_regular_file(candidate, error)) return canonical_path(candidate);
        }
        throw Diagnostic(entry.span, "schema module '" + entry.module + "' not found (" + rel + ")");
    }

    Document &module(const Import &entry, const fs::path &from) {
        auto path = find_file(entry, from);
        auto existing = module_paths.emplace(entry.module, path);
        if (!existing.second && existing.first->second != path)
            throw Diagnostic(entry.span, "schema module '" + entry.module + "' resolves to multiple files");
        if (auto found = files.find(path); found != files.end()) {
            if (!found->second->active) return *found->second;
            std::string cycle;
            for (auto i = std::find(stack.begin(), stack.end(), path); i != stack.end(); ++i)
                cycle += i->generic_string() + " -> ";
            throw Diagnostic(entry.span, "schema import cycle: " + cycle + path.generic_string());
        }
        if (stack.size() >= 128)
            throw Diagnostic(entry.span, "schema import nesting exceeds 128 levels");
        Schema schema;
        try {
            schema = parse(read_file(path), path.string());
        } catch (const std::system_error &error) {
            throw Diagnostic(entry.span, error.what());
        }
        if (!schema.modular)
            throw Diagnostic(entry.span, "imported schema module '" + entry.module +
                                         "' must declare .modular #true");
        documents.push_back(std::make_unique<Document>(std::move(schema), encoded_module(entry.module)));
        auto &doc = *documents.back();
        files.emplace(path, &doc);
        walk(doc, path);
        return doc;
    }

    void walk(Document &doc, const fs::path &path) {
        stack.push_back(path);
        for (const auto &entry : doc.schema.imports) {
            if (!entry.schema) {
                doc.add_lua_import(entry);
                continue;
            }
            auto &target = module(entry, path);
            if (entry.names.empty()) {
                auto add_namespace = [&](const std::string &alias) {
                    if (!doc.namespaces.emplace(alias, &target).second)
                        throw Diagnostic(entry.span, "duplicate schema import alias '" + alias + "'");
                };
                auto alias = alias_name(entry);
                add_namespace(alias);
                if (entry.alias.empty() && alias != entry.module) add_namespace(entry.module);
            } else {
                for (const auto &name : entry.names) {
                    auto found = target.local.find(name.name);
                    if (found == target.local.end())
                        throw Diagnostic(name.span, "schema module '" + entry.module +
                                                    "' does not define '" + name.name + "'");
                    auto alias = local_name(name);
                    if (!grammar::lower(alias[0]))
                        throw Diagnostic(name.span, "a schema type alias must start with lowercase");
                    if (primitives().count(alias) || doc.local.count(alias) ||
                        !doc.selected.emplace(alias, found->second).second)
                        throw Diagnostic(name.span, "duplicate schema type binding '" + alias + "'");
                }
            }
        }
        doc.rewrite();
        stack.pop_back();
        doc.active = false;
    }
};

} // namespace

Schema parse_file(const fs::path &path, const std::vector<fs::path> &import_roots) {
    auto schema = parse(read_file(path), path.string());
    resolve(schema, import_roots);
    return schema;
}

void resolve(Schema &schema, const std::vector<fs::path> &import_roots) {
    if (!schema.resolved) schema = Resolver(import_roots).run(schema);
}

} // namespace asdl
