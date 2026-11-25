#include "WASM-Instance.hpp"

#include "WASM-Operations.hpp"
#include "WASM-Validate.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

constexpr auto no_pc = std::numeric_limits<std::size_t>::max();

struct Delimiters {
    std::size_t else_pc = no_pc;
    std::size_t end_pc = no_pc;
};

struct FunctionPlan {
    std::size_t locals;
    std::vector<Delimiters> delimiters;
};

// Validation has already checked nesting. Record destinations once so jumping
// over an untaken arm or repeatedly entering a loop never scans its body.
std::vector<Delimiters> find_delimiters(std::span<const Instruction> code) {
    std::vector<Delimiters> result(code.size());
    std::vector<std::size_t> open;
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
        switch (code[pc].opcode.byte) {
        case 0x02: // block
        case 0x03: // loop
        case 0x04: // if
            open.push_back(pc);
            break;
        case 0x05: // else
            result[open.back()].else_pc = pc;
            break;
        case 0x0b: // end
            if (!open.empty()) {
                result[open.back()].end_pc = pc;
                open.pop_back();
            }
            break;
        }
    }
    return result;
}

struct Label {
    std::size_t height, head, end;
    std::size_t params, results;
    bool loop;
};

struct Frame {
    std::size_t function; // Index in the defined functions, excluding imports.
    std::size_t pc{};
    std::vector<Value> locals, stack;
    std::vector<Label> labels;
};

Value pop(std::vector<Value>& stack) {
    const auto value = stack.back();
    stack.pop_back();
    return value;
}

std::vector<Value> take_results(std::vector<Value>& stack, std::size_t arity) {
    const auto base = stack.size() - arity;
    const auto tail = std::span(stack).last(arity);
    std::vector<Value> values(tail.begin(), tail.end());
    stack.resize(base);
    return values;
}

// Keep the top arity values, discarding values introduced since the destination
// label's entry. The destination starts no higher than the source.
void unwind_stack(std::vector<Value>& stack, std::size_t height, std::size_t arity) {
    const auto base = stack.size() - arity;
    if (height != base)
        for (std::size_t i = 0; i < arity; ++i)
            stack[height + i] = stack[base + i];
    stack.resize(height + arity);
}

void branch(Frame& frame, std::size_t depth) {
    const auto destination = frame.labels.size() - 1 - depth;
    const auto label = frame.labels[destination];
    if (label.loop) {
        unwind_stack(frame.stack, label.height, label.params);
        frame.labels.resize(destination + 1);
        frame.pc = label.head;
    } else {
        unwind_stack(frame.stack, label.height, label.results);
        frame.labels.resize(destination);
        frame.pc = label.end + 1;
    }
}

void close_label(Frame& frame) {
    const auto label = frame.labels.back();
    unwind_stack(frame.stack, label.height, label.results);
    frame.labels.pop_back();
    frame.pc = label.end + 1;
}

void spend_fuel(std::uint64_t& fuel, std::size_t offset) {
    if (fuel == 0)
        throw Trap(offset, "execution ran out of fuel");
    --fuel;
}

std::string_view kind_name(ExternalKind kind) {
    switch (kind) {
    case ExternalKind::function: return "function";
    case ExternalKind::table: return "table";
    case ExternalKind::memory: return "memory";
    case ExternalKind::global: return "global";
    }
    return {};
}

std::size_t memory_start(std::size_t size, std::uint64_t offset, std::size_t count) {
    // Subtraction after checking offset also rejects ranges whose end would wrap.
    if (offset > size || count > size - offset)
        throw Error(0, "out-of-bounds memory access");
    return static_cast<std::size_t>(offset);
}

std::size_t table_slot(std::size_t size, std::uint64_t index) {
    if (index >= size)
        throw Error(0, "out-of-bounds table index");
    return static_cast<std::size_t>(index);
}

std::vector<std::uint8_t> read_bytes(std::span<const std::uint8_t> storage, std::uint64_t offset,
                                     std::size_t count) {
    const auto start = memory_start(storage.size(), offset, count);
    if (count == 0)
        return {};
    const auto range = storage.subspan(start, count);
    return {range.begin(), range.end()};
}

void write_bytes(std::span<std::uint8_t> storage, std::uint64_t offset,
                 std::span<const std::uint8_t> bytes) {
    const auto start = memory_start(storage.size(), offset, bytes.size());
    if (!bytes.empty())
        std::ranges::copy(bytes, storage.subspan(start, bytes.size()).begin());
}

bool match_limits(Limits expected, Limits actual) {
    // Runtime types use the current size and the *declared* maximum, not the
    // implementation's backing-store cap.
    return actual.min >= expected.min &&
        (!expected.max || (actual.max && *actual.max <= *expected.max));
}

} // namespace

HostMemory::HostMemory(MemoryType type)
    : storage_(std::make_shared<detail::MemoryInstance>(type)) {}

MemoryType HostMemory::type() const noexcept {
    return storage_->type();
}

std::vector<std::uint8_t> HostMemory::read(std::uint64_t offset, std::size_t count) const {
    return read_bytes(storage_->bytes, offset, count);
}

void HostMemory::write(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    write_bytes(storage_->bytes, offset, bytes);
}

HostTable::HostTable(TableType type)
    : storage_(std::make_shared<detail::TableInstance>(type)) {}

TableType HostTable::type() const noexcept {
    return storage_->type();
}

Value HostTable::get(std::uint64_t index) const {
    return storage_->entries[table_slot(storage_->entries.size(), index)];
}

void HostTable::set(std::uint64_t index, Value value) {
    const auto slot = table_slot(storage_->entries.size(), index);
    if (!detail::match_value(storage_->element, value))
        throw Error(0, "host table element type mismatch or non-null funcref");
    storage_->entries[slot] = value;
}

HostGlobal::HostGlobal(GlobalType type, Value initial) {
    if (!detail::match_value(type.type, initial))
        throw Error(0, "host global initial value type mismatch or non-null funcref");
    storage_ = std::make_shared<detail::GlobalInstance>(detail::GlobalInstance{type, initial});
}

GlobalType HostGlobal::type() const noexcept {
    return storage_->type;
}

Value HostGlobal::get() const noexcept {
    return storage_->value;
}

void HostGlobal::set(Value value) {
    if (!storage_->type.mutable_)
        throw Error(0, "cannot set an immutable host global");
    if (!detail::match_value(storage_->type.type, value))
        throw Error(0, "host global value type mismatch or non-null funcref");
    storage_->value = value;
}

struct Instance::Impl {
    Module module;
    InstanceOptions options;
    detail::InstanceState state;
    std::vector<std::uint32_t> signatures;
    std::vector<std::size_t> function_imports;
    std::vector<HostCallback> callbacks;
    std::vector<FunctionPlan> plans;
    bool running{};

    Impl(Module source, HostImports bindings, InstanceOptions config)
        : module(std::move(source)), options(config) {
        validate_module(module);
        bind_imports(bindings);
        for (const auto& function : module.functions) {
            signatures.push_back(function.type);
            auto count = module.types[function.type].params.size();
            if (count > options.max_locals)
                throw Error(function.body.offset, "function local count exceeds instance limit");
            for (const auto& local : function.body.locals) {
                if (local.count > options.max_locals - count)
                    throw Error(function.body.offset, "function local count exceeds instance limit");
                count += local.count;
            }
            plans.push_back({count, find_delimiters(function.body.instructions)});
        }
        detail::initialize_state(module, state);
        if (module.start)
            run(*module.start, {}, options.start_fuel);
    }

    void bind_imports(HostImports bindings) {
        using Key = std::pair<std::string_view, std::string_view>;
        using Binding = std::variant<std::size_t, const HostObjectImport*>;
        std::map<Key, Binding> resolver;
        for (const auto& binding : bindings.functions) {
            if (!binding.callback)
                throw Error(0, "empty host function binding: " + binding.module + "." + binding.name);
            if (!resolver.emplace(Key{binding.module, binding.name}, callbacks.size()).second)
                throw Error(0, "duplicate host function binding: " + binding.module + "." + binding.name);
            callbacks.push_back(binding.callback);
        }
        for (const auto& binding : bindings.objects)
            if (!resolver.emplace(Key{binding.module, binding.name}, &binding).second)
                throw Error(0, "duplicate host import binding: " + binding.module + "." + binding.name);

        for (const auto& import : module.imports) {
            const auto kind = static_cast<ExternalKind>(import.desc.index());
            const auto name = import.module + "." + import.name;
            const auto binding = resolver.find({import.module, import.name});
            if (binding == resolver.end())
                throw Error(0, "missing host " + std::string(kind_name(kind)) + " binding: " + name);
            const auto wrong_kind = [&] {
                return Error(0, "wrong kind for host " + std::string(kind_name(kind)) + " binding: " + name);
            };
            if (const auto* callback = std::get_if<std::size_t>(&binding->second)) {
                if (kind != ExternalKind::function)
                    throw wrong_kind();
                signatures.push_back(std::get<std::uint32_t>(import.desc));
                function_imports.push_back(*callback);
                continue;
            }
            const auto& object = std::get<const HostObjectImport*>(binding->second)->object;
            if (const auto* memory_type = std::get_if<MemoryType>(&import.desc)) {
                const auto* memory = std::get_if<HostMemory>(&object);
                if (!memory)
                    throw wrong_kind();
                if (!match_limits(memory_type->limits, memory->type().limits))
                    throw Error(0, "host memory limits mismatch: " + name);
                state.memory = memory->storage_;
            } else if (const auto* table_type = std::get_if<TableType>(&import.desc)) {
                const auto* table = std::get_if<HostTable>(&object);
                if (!table)
                    throw wrong_kind();
                const auto actual = table->type();
                if (table_type->element != actual.element)
                    throw Error(0, "host table element type mismatch: " + name);
                if (!match_limits(table_type->limits, actual.limits))
                    throw Error(0, "host table limits mismatch: " + name);
                state.attach_table(table->storage_, name);
            } else if (const auto* global_type = std::get_if<GlobalType>(&import.desc)) {
                const auto* global = std::get_if<HostGlobal>(&object);
                if (!global)
                    throw wrong_kind();
                if (*global_type != global->type())
                    throw Error(0, "host global type or mutability mismatch: " + name);
                state.attach_global(global->storage_, name);
            } else {
                throw wrong_kind();
            }
        }
    }

    const FunctionType& signature(std::uint32_t function) const {
        return module.types[signatures[function]];
    }

    std::uint32_t export_index(std::string_view name, ExternalKind kind) const {
        const auto entry = std::ranges::find(module.exports, name, &Export::name);
        if (entry == module.exports.end() || entry->kind != kind)
            throw Error(0, "unknown " + std::string(kind_name(kind)) + " export: " + std::string(name));
        return entry->index;
    }

    bool match_value(Type type, const Value& value) const {
        return detail::match_value(type, value, signatures.size());
    }

    bool match_values(std::span<const Type> types, std::span<const Value> values) const {
        if (types.size() != values.size())
            return false;
        for (std::size_t i = 0; i < types.size(); ++i)
            if (!match_value(types[i], values[i]))
                return false;
        return true;
    }

    std::vector<Value> invoke_import(std::uint32_t function, std::span<const Value> args,
                                     std::size_t offset) {
        std::vector<Value> values;
        try {
            values = callbacks[function_imports[function]](args);
        } catch (const std::exception& error) {
            throw Trap(offset, std::string("host function failed: ") + error.what());
        } catch (...) {
            throw Trap(offset, "host function failed");
        }
        if (!match_values(signature(function).results, values))
            throw Trap(offset, "host function result type mismatch");
        return values;
    }

    void push_frame(std::vector<Frame>& frames, std::size_t& live_locals, std::uint32_t function,
                    std::vector<Value> args, std::size_t offset) {
        if (frames.size() == options.max_call_depth)
            throw Trap(offset, "function call depth exceeds instance limit");
        const auto index = function - function_imports.size();
        const auto& body = module.functions[index].body;
        const auto count = plans[index].locals;
        if (count > options.max_live_locals - live_locals)
            throw Trap(offset, "live function local count exceeds instance limit");
        Frame frame{index, 0, std::move(args), {}, {}};
        frame.locals.reserve(count);
        for (const auto& local : body.locals)
            frame.locals.resize(frame.locals.size() + local.count, Value{local.type, 0});
        frame.labels.push_back({0, 0, body.instructions.size() - 1,
                                0, signature(function).results.size(), false});
        frames.push_back(std::move(frame));
        live_locals += count;
    }

    std::pair<std::size_t, std::size_t> block_arities(const BlockType& type) const {
        if (std::holds_alternative<Type>(type.value))
            return {0, 1};
        if (const auto* index = std::get_if<std::uint32_t>(&type.value)) {
            const auto& signature = module.types[*index];
            return {signature.params.size(), signature.results.size()};
        }
        return {0, 0};
    }

    std::uint32_t indirect_target(IndexPair imm, std::uint32_t selector, std::size_t offset) const {
        const auto& entries = state.tables[imm.second]->entries;
        if (selector >= entries.size())
            throw Trap(offset, "out-of-bounds indirect call table index");
        const auto bits = entries[selector].bits;
        if (bits == 0)
            throw Trap(offset, "indirect call to a null function reference");
        const auto target = static_cast<std::uint32_t>(bits - 1);
        const auto& actual = signature(target);
        const auto& expected = module.types[imm.first];
        if (actual != expected)
            throw Trap(offset, "indirect call type mismatch");
        return target;
    }

    std::vector<Value> run(std::uint32_t function, std::span<const Value> args, std::uint64_t fuel) {
        if (running)
            throw Error(0, "reentrant invocation of the same instance is not supported");
        if (function >= signatures.size())
            throw Error(0, "unknown function index");
        if (!match_values(signature(function).params, args))
            throw Error(0, "function argument type mismatch");
        running = true;
        struct Reset {
            bool& running;
            ~Reset() { running = false; }
        } reset{running};
        if (function < function_imports.size()) {
            spend_fuel(fuel, 0);
            return invoke_import(function, args, 0);
        }
        std::vector<Frame> frames;
        std::size_t live_locals = 0;
        const auto offset = module.functions[function - function_imports.size()].body.offset;
        push_frame(frames, live_locals, function, {args.begin(), args.end()}, offset);
        for (;;) {
            auto& frame = frames.back();
            auto& stack = frame.stack;
            const auto& body = module.functions[frame.function].body;
            if (frame.pc == body.instructions.size()) {
                auto results = std::move(stack);
                live_locals -= frame.locals.size();
                frames.pop_back();
                if (frames.empty())
                    return results;
                auto& caller = frames.back().stack;
                caller.insert(caller.end(), results.begin(), results.end());
                continue;
            }
            const auto pc = frame.pc++;
            const auto& instruction = body.instructions[pc];
            spend_fuel(fuel, instruction.offset);
            const auto index = [&] { return std::get<std::uint32_t>(instruction.immediate); };
            switch (instruction.opcode.byte) {
            case 0x00: // unreachable
                throw Trap(instruction.offset, "unreachable instruction executed");
            case 0x01: // nop
                break;
            case 0x02: // block
            case 0x03: // loop
            case 0x04: { // if
                const auto opcode = instruction.opcode.byte;
                const auto taken = opcode != 0x04 || pop(stack).bits != 0;
                const auto [params, results] = block_arities(std::get<BlockType>(instruction.immediate));
                const auto [else_pc, end_pc] = plans[frame.function].delimiters[pc];
                frame.labels.push_back({stack.size() - params, pc + 1, end_pc,
                                        params, results, opcode == 0x03});
                if (!taken)
                    frame.pc = else_pc == no_pc ? end_pc : else_pc + 1;
                break;
            }
            case 0x05: // else (the then arm has finished)
            case 0x0b: // end
                close_label(frame);
                break;
            case 0x0c: // br
                branch(frame, index());
                break;
            case 0x0d: // br_if
                if (pop(stack).bits != 0)
                    branch(frame, index());
                break;
            case 0x0e: { // br_table
                const auto& table = std::get<BranchTable>(instruction.immediate);
                const auto selector = static_cast<std::uint32_t>(pop(stack).bits);
                branch(frame, selector < table.labels.size() ? table.labels[selector] : table.otherwise);
                break;
            }
            case 0x0f: // return
                branch(frame, frame.labels.size() - 1);
                break;
            case 0x10: // call
            case 0x11: { // call_indirect
                const auto target = instruction.opcode.byte == 0x10 ? index()
                    : indirect_target(std::get<IndexPair>(instruction.immediate),
                                      static_cast<std::uint32_t>(pop(stack).bits), instruction.offset);
                auto call_args = take_results(stack, signature(target).params.size());
                if (target < function_imports.size()) {
                    const auto results = invoke_import(target, call_args, instruction.offset);
                    stack.insert(stack.end(), results.begin(), results.end());
                } else {
                    push_frame(frames, live_locals, target, std::move(call_args), instruction.offset);
                }
                break;
            }
            case 0x1a: // drop
                stack.pop_back();
                break;
            case 0x1b: // select
            case 0x1c: { // typed select
                const auto condition = pop(stack).bits;
                const auto other = pop(stack);
                if (condition == 0)
                    stack.back() = other;
                break;
            }
            case 0x20: // local.get
                stack.push_back(frame.locals[index()]);
                break;
            case 0x21: // local.set
                frame.locals[index()] = pop(stack);
                break;
            case 0x22: // local.tee
                frame.locals[index()] = stack.back();
                break;
            default:
                if (!detail::execute_opcode(instruction, stack, state))
                    throw Error(instruction.offset, "unsupported instruction in module interpreter");
                break;
            }
        }
    }
};

Instance::Instance(Module module, std::span<const HostFunction> imports, InstanceOptions options)
    : Instance(std::move(module), HostImports{imports}, options, BindObjects{}) {}

Instance::Instance(Module module, HostImports imports, InstanceOptions options, BindObjects)
    : impl_(std::make_unique<Impl>(std::move(module), imports, options)) {}

Instance Instance::with_imports(Module module, HostImports imports, InstanceOptions options) {
    return Instance(std::move(module), imports, options, BindObjects{});
}

Instance::~Instance() = default;
Instance::Instance(Instance&&) noexcept = default;
Instance& Instance::operator=(Instance&&) noexcept = default;

std::vector<Value> Instance::invoke(std::uint32_t function, std::span<const Value> args,
                                    std::uint64_t fuel) {
    if (!impl_)
        throw Error(0, "cannot invoke a moved-from instance");
    return impl_->run(function, args, fuel);
}

std::vector<Value> Instance::invoke(std::string_view name, std::span<const Value> args,
                                    std::uint64_t fuel) {
    if (!impl_)
        throw Error(0, "cannot invoke a moved-from instance");
    return impl_->run(impl_->export_index(name, ExternalKind::function), args, fuel);
}

Instance::Impl& Instance::export_instance() const {
    if (!impl_)
        throw Error(0, "cannot access exports of a moved-from instance");
    return *impl_;
}

std::vector<std::uint8_t> Instance::read_memory(std::string_view name, std::uint64_t offset,
                                                std::size_t count) const {
    const auto& instance = export_instance();
    instance.export_index(name, ExternalKind::memory);
    // Validated Core 2.0 modules have at most one memory.
    return read_bytes(instance.state.memory_bytes(), offset, count);
}

void Instance::write_memory(std::string_view name, std::uint64_t offset,
                            std::span<const std::uint8_t> bytes) {
    auto& instance = export_instance();
    instance.export_index(name, ExternalKind::memory);
    write_bytes(instance.state.memory_bytes(), offset, bytes);
}

Value Instance::get_global(std::string_view name) const {
    const auto& instance = export_instance();
    return instance.state.globals[instance.export_index(name, ExternalKind::global)]->value;
}

void Instance::set_global(std::string_view name, Value value) {
    auto& instance = export_instance();
    auto& global = *instance.state.globals[instance.export_index(name, ExternalKind::global)];
    if (!global.type.mutable_)
        throw Error(0, "cannot set an immutable global export: " + std::string(name));
    if (!instance.match_value(global.type.type, value))
        throw Error(0, "global export value type mismatch: " + std::string(name));
    global.value = value;
}

Value Instance::get_table(std::string_view name, std::uint64_t index) const {
    const auto& instance = export_instance();
    const auto& entries = instance.state.tables[instance.export_index(name, ExternalKind::table)]->entries;
    return entries[table_slot(entries.size(), index)];
}

void Instance::set_table(std::string_view name, std::uint64_t index, Value value) {
    auto& instance = export_instance();
    auto& table = *instance.state.tables[instance.export_index(name, ExternalKind::table)];
    const auto slot = table_slot(table.entries.size(), index);
    if (!instance.match_value(table.element, value))
        throw Error(0, "table export element type mismatch: " + std::string(name));
    table.entries[slot] = value;
}

} // namespace limestone::wasmutils::core
