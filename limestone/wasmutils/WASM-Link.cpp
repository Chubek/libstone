#include "WASM-Link.hpp"
#include "WASM-Validate.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

using Kind = ExternalKind;
using Index = std::uint32_t;
using Desc = ImportDesc;

constexpr std::array kinds{Kind::function, Kind::table, Kind::memory, Kind::global};

constexpr std::size_t space(Kind kind) {
  return static_cast<std::size_t>(kind);
}

std::string_view kind_name(Kind kind) {
  switch (kind) {
  case Kind::function: return "function";
  case Kind::table: return "table";
  case Kind::memory: return "memory";
  case Kind::global: return "global";
  }
  return "unknown";
}

std::string symbol_name(const Import& import) {
  return "'" + import.module + "." + import.name + "'";
}

[[noreturn]] void fail(std::size_t module, const std::string& message, std::size_t offset = 0) {
  throw Error(offset, "link input " + std::to_string(module) + ": " + message);
}

Index checked_index(std::size_t index) {
  if (index > std::numeric_limits<Index>::max())
    throw Error(0, "linked module exceeds the Core 2.0 index range");
  return static_cast<Index>(index);
}

struct Ref {
  std::size_t module;
  Kind kind;
  Index index;
};

struct Slot {
  const Import* import = nullptr;
  std::optional<Ref> target;
  std::optional<Index> output;
  bool visiting = false;
};

struct Input {
  std::array<std::vector<Slot>, kinds.size()> entities;
  std::array<std::size_t, kinds.size()> imports{};
  std::map<std::string_view, Ref> exports;
  std::vector<Index> types;
  std::size_t element_base = 0;
  std::size_t data_base = 0;
};

struct TypeLess {
  bool operator()(const FunctionType& left, const FunctionType& right) const {
    return std::tie(left.params, left.results) < std::tie(right.params, right.results);
  }
};

bool limits_match(const Limits& expected, const Limits& actual) {
  return actual.min >= expected.min &&
         (!expected.max || (actual.max && *actual.max <= *expected.max));
}

bool limits_intersect(Limits& left, const Limits& right) {
  left.min = std::max(left.min, right.min);
  if (right.max && (!left.max || *right.max < *left.max))
    left.max = right.max;
  return !left.max || left.min <= *left.max;
}

template<class MatchLimits>
bool descriptors_match(Desc& expected, const Desc& actual, MatchLimits match_limits) {
  if (expected.index() != actual.index())
    return false;
  if (auto* function = std::get_if<Index>(&expected))
    return *function == std::get<Index>(actual);
  if (auto* table = std::get_if<TableType>(&expected)) {
    const auto& other = std::get<TableType>(actual);
    return table->element == other.element && match_limits(table->limits, other.limits);
  }
  if (auto* memory = std::get_if<MemoryType>(&expected))
    return match_limits(memory->limits, std::get<MemoryType>(actual).limits);
  const auto& global = std::get<GlobalType>(expected);
  const auto& other = std::get<GlobalType>(actual);
  return global.type == other.type && global.mutable_ == other.mutable_;
}

class Linker {
public:
  explicit Linker(std::span<const Module> modules)
      : inputs_(modules.size()) {
    modules_.reserve(modules.size());
    for (const auto& module : modules)
      modules_.push_back({{}, &module});
  }

  explicit Linker(std::span<const LinkInput> inputs)
      : named_(true), modules_(inputs.begin(), inputs.end()), inputs_(inputs.size()) {
    for (std::size_t m = 0; m < modules_.size(); ++m) {
      const auto& input = modules_[m];
      if (!input.module)
        fail(m, "missing named input module");
      if (input.name.empty())
        fail(m, "input namespace must not be empty");
      if (!namespaces_.emplace(input.name, m).second)
        fail(m, "duplicate input namespace '" + std::string(input.name) + "'");
    }
  }

  Module run() {
    collect();
    match_imports();
    allocate_indices();
    resolve_imports();
    append_content();
    append_start();
    validate_module(output_);
    return std::move(output_);
  }

private:
  struct ExportName {
    Ref first;
    bool repeated{};
  };

  bool named_ = false;
  std::vector<LinkInput> modules_;
  std::vector<Input> inputs_;
  Module output_;
  std::map<FunctionType, Index, TypeLess> types_;
  std::map<std::string_view, std::size_t> namespaces_;
  std::map<std::string_view, ExportName> exports_;
  std::array<std::size_t, kinds.size()> external_counts_{};
  std::vector<Ref> global_definitions_;
  std::vector<std::optional<Program>> global_initializers_;

  const Module& module_at(std::size_t m) const {
    return *modules_[m].module;
  }

  Slot& slot(Ref ref) {
    return inputs_[ref.module].entities[space(ref.kind)][ref.index];
  }

  const Slot& slot(Ref ref) const {
    return inputs_[ref.module].entities[space(ref.kind)][ref.index];
  }

  Index intern_type(const FunctionType& type) {
    const auto [it, inserted] = types_.try_emplace(type, checked_index(output_.types.size()));
    if (inserted)
      output_.types.push_back(type);
    return it->second;
  }

  void collect() {
    std::size_t elements = 0, data = 0;
    for (std::size_t m = 0; m < modules_.size(); ++m) {
      const auto& module = module_at(m);
      for (const auto& custom : module.custom) {
        if (custom.name == "linking" || custom.name.starts_with("reloc."))
          fail(m, "unsupported Wasm object section '" + custom.name +
                      "' in a decoded module; use link_objects with the original bytes");
        if (custom.name == "dylink" || custom.name.starts_with("dylink."))
          fail(m, "unsupported Wasm dynamic-linking section '" + custom.name + "'");
      }
      try {
        validate_module(module);
      } catch (const Error& error) {
        fail(m, error.what(), error.offset);
      }

      auto& input = inputs_[m];
      for (const auto& type : module.types)
        input.types.push_back(intern_type(type));
      for (const auto& import : module.imports) {
        auto& slots = input.entities[import.desc.index()];
        slots.emplace_back().import = &import;
        ++input.imports[import.desc.index()];
      }
      const std::array sizes{module.functions.size(), module.tables.size(),
                             module.memories.size(), module.globals.size()};
      for (const auto kind : kinds) {
        const auto k = space(kind);
        input.entities[k].resize(input.imports[k] + sizes[k]);
      }
      input.element_base = elements;
      input.data_base = data;
      elements += module.elements.size();
      data += module.data.size();
      checked_index(elements);
      checked_index(data);
      if (module.data_count)
        output_.data_count = 0;

      for (const auto& export_ : module.exports) {
        const Ref ref{m, export_.kind, export_.index};
        if (named_)
          input.exports.emplace(export_.name, ref);
        const auto [it, inserted] = exports_.try_emplace(
            export_.name, ExportName{ref});
        if (inserted)
          continue;
        if (!named_)
          fail(m, "duplicate export '" + export_.name + "' (first exported by input " +
                      std::to_string(it->second.first.module) + ")");
        it->second.repeated = true;
      }
    }
    if (output_.data_count)
      output_.data_count = checked_index(data);
  }

  Desc descriptor(Ref ref) const {
    const auto& input = inputs_[ref.module];
    if (const auto* import = slot(ref).import) {
      if (const auto* type = std::get_if<Index>(&import->desc))
        return input.types[*type];
      return import->desc;
    }
    const auto& module = module_at(ref.module);
    const auto index = ref.index - input.imports[space(ref.kind)];
    switch (ref.kind) {
    case Kind::function: return input.types[module.functions[index].type];
    case Kind::table: return module.tables[index];
    case Kind::memory: return module.memories[index];
    case Kind::global: return module.globals[index].type;
    }
    fail(ref.module, "unknown symbol kind");
  }

  template<class Visit>
  void for_each_import(Visit visit) {
    for (std::size_t m = 0; m < modules_.size(); ++m) {
      std::array<Index, kinds.size()> indices{};
      // Preserve the original mixed-kind ordering of imports.
      for (const auto& import : module_at(m).imports) {
        const auto k = import.desc.index();
        const Ref ref{m, kinds[k], indices[k]++};
        visit(ref, import, slot(ref));
      }
    }
  }

  std::optional<Ref> provider_for(Ref ref, const Import& import) const {
    if (named_) {
      const auto ns = namespaces_.find(import.module);
      if (ns == namespaces_.end())
        return std::nullopt;
      const auto& fields = inputs_[ns->second].exports;
      if (const auto it = fields.find(import.name); it != fields.end())
        return it->second;
      fail(ref.module, "missing export for import " + symbol_name(import) +
          " in input " + std::to_string(ns->second));
    }
    const auto it = exports_.find(import.name);
    if (it == exports_.end() || it->second.first.module == ref.module)
      return std::nullopt;
    return it->second.first;
  }

  void match_imports() {
    for_each_import([&](Ref ref, const Import& import, Slot& entry) {
      const auto resolved = provider_for(ref, import);
      if (!resolved)
        return;
      const auto target = *resolved;
      const auto provider = " from input " + std::to_string(target.module);
      if (target.kind != ref.kind)
        fail(ref.module, "import " + symbol_name(import) + " expects a " +
            std::string(kind_name(ref.kind)) + " but matches a " +
            std::string(kind_name(target.kind)) + " export" + provider);
      auto expected = descriptor(ref);
      if (!descriptors_match(expected, descriptor(target), limits_match))
        fail(ref.module, "incompatible " + std::string(kind_name(ref.kind)) + " import " +
            symbol_name(import) + " and export" + provider);
      entry.target = target;
    });
  }

  void allocate_indices() {
    struct HostImport {
      std::size_t entry;
      Index index;
    };
    std::map<std::pair<std::string_view, std::string_view>, HostImport> host;
    for_each_import([&](Ref ref, const Import& import, Slot& entry) {
      if (entry.target)
        return;
      const auto k = space(ref.kind);
      const auto [it, inserted] = host.try_emplace(std::pair{std::string_view(import.module),
          std::string_view(import.name)}, HostImport{output_.imports.size(),
              checked_index(external_counts_[k])});
      auto desc = descriptor(ref);
      if (inserted) {
        output_.imports.push_back({import.module, import.name, std::move(desc)});
        ++external_counts_[k];
      } else if (!descriptors_match(output_.imports[it->second.entry].desc, desc, limits_intersect)) {
        fail(ref.module, "conflicting host import requirements for " + symbol_name(import));
      }
      entry.output = it->second.index;
    });

    auto next = external_counts_;
    for (std::size_t m = 0; m < inputs_.size(); ++m) {
      auto& input = inputs_[m];
      for (const auto kind : kinds) {
        const auto k = space(kind);
        for (std::size_t i = input.imports[k]; i < input.entities[k].size(); ++i) {
          input.entities[k][i].output = checked_index(next[k]++);
          if (kind == Kind::global)
            global_definitions_.push_back({m, kind, checked_index(i)});
        }
      }
    }
    for (const auto size : next)
      checked_index(size);
    if (next[space(Kind::memory)] > 1)
      throw Error(0, "cannot link independent memories: Core 2.0 supports only one memory");
    global_initializers_.resize(global_definitions_.size());
  }

  void resolve_imports() {
    std::vector<Ref> path;
    for_each_import([&](Ref current, const Import&, Slot&) {
      while (!slot(current).output) {
        auto& entry = slot(current);
        if (entry.visiting)
          fail(current.module, "cyclic import/export alias for " + symbol_name(*entry.import));
        entry.visiting = true;
        path.push_back(current);
        current = *entry.target;
      }
      for (const auto ref : path) {
        slot(ref).output = slot(current).output;
        slot(ref).visiting = false;
      }
      path.clear();
    });
  }

  Index mapped(std::size_t module, Kind kind, Index index) const {
    return *slot({module, kind, index}).output;
  }

  Program rewrite_program(Program program, std::size_t module) const {
    const auto& input = inputs_[module];
    for (auto& insn : program) {
      auto rewrite_index = [&](Kind kind) {
        auto& index = std::get<Index>(insn.immediate);
        index = mapped(module, kind, index);
      };
      switch (insn.opcode.byte) {
      case 0x02: case 0x03: case 0x04: // block, loop, if
        if (auto* type = std::get_if<Index>(&std::get<BlockType>(insn.immediate).value))
          *type = input.types[*type];
        break;
      case 0x10: case 0xd2: // call, ref.func
        rewrite_index(Kind::function);
        break;
      case 0x11: { // call_indirect (type, table)
        auto& pair = std::get<IndexPair>(insn.immediate);
        pair.first = input.types[pair.first];
        pair.second = mapped(module, Kind::table, pair.second);
        break;
      }
      case 0x23: case 0x24: // global.get, global.set
        rewrite_index(Kind::global);
        break;
      case 0x25: case 0x26: // table.get, table.set
        rewrite_index(Kind::table);
        break;
      case 0xfc:
        switch (insn.opcode.extension) {
        case 8: case 9: // memory.init, data.drop
          std::get<Index>(insn.immediate) += checked_index(input.data_base);
          break;
        case 12: { // table.init (element, table)
          auto& pair = std::get<IndexPair>(insn.immediate);
          pair.first += checked_index(input.element_base);
          pair.second = mapped(module, Kind::table, pair.second);
          break;
        }
        case 13: // elem.drop
          std::get<Index>(insn.immediate) += checked_index(input.element_base);
          break;
        case 14: { // table.copy (destination table, source table)
          auto& pair = std::get<IndexPair>(insn.immediate);
          pair.first = mapped(module, Kind::table, pair.first);
          pair.second = mapped(module, Kind::table, pair.second);
          break;
        }
        case 15: case 16: case 17: // table.grow, table.size, table.fill
          rewrite_index(Kind::table);
          break;
        }
        break;
      }
      // Core 2.0 always addresses memory zero. The single-memory check above
      // ensures both explicit zero operands and implicit memory references stay valid.
    }
    return program;
  }

  // Core 2.0 constant expressions can read imported immutable globals, but
  // cannot read definitions. Inlining the definition keeps the result valid.
  std::optional<std::size_t> constant_dependency(const Program& program,
                                                 std::size_t module) const {
    if (program.size() != 2 || program.front().opcode != Opcode{0x23})
      return std::nullopt;
    const auto index = mapped(module, Kind::global, std::get<Index>(program.front().immediate));
    const auto imports = external_counts_[space(Kind::global)];
    if (index < imports)
      return std::nullopt;
    return index - imports;
  }

  const Program& global_initializer(std::size_t index) {
    const auto requested = index;
    std::vector<std::size_t> path;
    while (!global_initializers_[index]) {
      const auto ref = global_definitions_[index];
      const auto local = ref.index - inputs_[ref.module].imports[space(Kind::global)];
      const auto& init = module_at(ref.module).globals[local].init;
      const auto dependency = constant_dependency(init, ref.module);
      if (!dependency) {
        global_initializers_[index] = rewrite_program(init, ref.module);
        break;
      }
      if (path.size() == global_definitions_.size())
        fail(ref.module, "cyclic linked global initializer", init.front().offset);
      path.push_back(index);
      index = *dependency;
    }
    for (const auto pending : path)
      global_initializers_[pending] = *global_initializers_[index];
    return *global_initializers_[requested];
  }

  Program rewrite_constant(Program program, std::size_t module) {
    if (const auto dependency = constant_dependency(program, module)) {
      auto result = global_initializer(*dependency);
      result.front().offset = program.front().offset;
      result.back().offset = program.back().offset;
      return result;
    }
    return rewrite_program(std::move(program), module);
  }

  void append_content() {
    std::map<std::string, std::size_t> qualified;
    for (std::size_t m = 0; m < modules_.size(); ++m) {
      const auto& module = module_at(m);
      for (auto function : module.functions) {
        function.type = inputs_[m].types[function.type];
        function.body.instructions = rewrite_program(std::move(function.body.instructions), m);
        output_.functions.push_back(std::move(function));
      }
      output_.tables.insert(output_.tables.end(), module.tables.begin(), module.tables.end());
      output_.memories.insert(output_.memories.end(), module.memories.begin(), module.memories.end());
      for (auto global : module.globals) {
        global.init = rewrite_constant(std::move(global.init), m);
        output_.globals.push_back(std::move(global));
      }
      for (auto export_ : module.exports) {
        export_.index = mapped(m, export_.kind, export_.index);
        if (named_) {
          if (exports_.at(export_.name).repeated)
            export_.name = std::string(modules_[m].name) + "." + export_.name;
          const auto [it, inserted] = qualified.try_emplace(export_.name, m);
          if (!inserted)
            fail(m, "output export collision '" + export_.name + "' (also exported by input " +
                      std::to_string(it->second) + ")");
        }
        output_.exports.push_back(std::move(export_));
      }
      for (auto element : module.elements) {
        if (element.mode == SegmentMode::active) {
          element.table = mapped(m, Kind::table, element.table);
          element.offset = rewrite_constant(std::move(element.offset), m);
        }
        for (auto& expr : element.init)
          expr = rewrite_constant(std::move(expr), m);
        output_.elements.push_back(std::move(element));
      }
      for (auto data : module.data) {
        if (data.mode == SegmentMode::active) {
          data.memory = mapped(m, Kind::memory, data.memory);
          data.offset = rewrite_constant(std::move(data.offset), m);
        }
        output_.data.push_back(std::move(data));
      }
    }
  }

  void append_start() {
    Program starts;
    for (std::size_t m = 0; m < modules_.size(); ++m) {
      if (const auto start = module_at(m).start)
        starts.push_back({{0x10}, mapped(m, Kind::function, *start)});
    }
    if (starts.empty())
      return;
    if (starts.size() == 1) {
      output_.start = std::get<Index>(starts.front().immediate);
      return;
    }
    const auto next = external_counts_[space(Kind::function)] + output_.functions.size();
    checked_index(next + 1);
    output_.start = checked_index(next);
    starts.push_back({{0x0b}, {}});
    output_.functions.push_back({intern_type({}), {0, {}, std::move(starts)}});
  }
};

} // namespace

Module link_modules(std::span<const Module> modules) {
  return Linker(modules).run();
}

Module link_modules(std::span<const LinkInput> inputs) {
  return Linker(inputs).run();
}

} // namespace limestone::wasmutils::core
