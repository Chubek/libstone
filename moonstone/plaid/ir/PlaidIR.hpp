#pragma once
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace moonstone::plaid::ir {
enum class Dialect { Control, Concurrent, Parallel };
enum class Op {
    Capture, Resume, Suspend, Effect, Handle, Yield,
    Spawn, Await, Cancel, Join, Send, Receive, Select, ActorSend,
    ParallelFor, Reduce, VectorLoad, VectorStore, DeviceLaunch
};
struct Instruction {
    Op op;
    std::vector<unsigned> inputs;
    std::vector<unsigned> outputs;
    unsigned block = 0;
};
struct Program { std::vector<Instruction> instructions; };
struct ValidationResult { bool valid = true; std::vector<std::string> errors; };
inline Dialect dialect(Op op) {
    switch (op) {
    case Op::Capture: case Op::Resume: case Op::Suspend: case Op::Effect:
    case Op::Handle: case Op::Yield: return Dialect::Control;
    case Op::Spawn: case Op::Await: case Op::Cancel: case Op::Join:
    case Op::Send: case Op::Receive: case Op::Select: case Op::ActorSend: return Dialect::Concurrent;
    default: return Dialect::Parallel;
    }
}
inline ValidationResult validate(const Program& p) {
    ValidationResult result;
    std::set<unsigned> defined;
    for (std::size_t idx=0; idx<p.instructions.size(); ++idx) {
        const auto& inst = p.instructions[idx];
        auto err=[&](std::string s) { result.valid=false; result.errors.push_back("instruction "+std::to_string(idx)+": "+s); };
        for (auto id:inst.inputs) if (!defined.contains(id)) err("use before definition: "+std::to_string(id));
        for (auto id:inst.outputs) if (!defined.insert(id).second) err("duplicate definition: "+std::to_string(id));
        if ((inst.op==Op::Await || inst.op==Op::Resume || inst.op==Op::Send || inst.op==Op::Receive) && inst.inputs.empty())
            err("effectful operation lacks operand");
        if (inst.op==Op::ParallelFor && inst.inputs.size()<2)
            err("parallel for requires begin and end");
    }
    return result;
}
} // namespace moonstone::plaid::ir
