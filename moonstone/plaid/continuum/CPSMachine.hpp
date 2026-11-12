#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::plaid::continuum {
// A fully explicit, cloneable *pure integer* continuation machine. It is
// suitable as a small reference semantics for compiler-generated state graphs;
// it does not capture C++ call stacks, foreign handles, or mutable resources.
using Register = std::uint32_t;
using ContinuationId = std::uint32_t;
enum class Arithmetic { Constant, Copy, Add, Subtract };
struct Instruction {
    Arithmetic kind = Arithmetic::Constant;
    Register output = 0, left = 0, right = 0;
    std::int64_t constant = 0;
};
enum class Transfer { Jump, BranchNonzero, Yield, Return };
struct Node {
    ContinuationId id = 0;
    std::vector<Instruction> operations;
    Transfer transfer = Transfer::Return;
    ContinuationId next = 0, alternative = 0;
    Register condition_or_value = 0;
};
struct Program {
    ContinuationId entry = 0;
    std::map<ContinuationId, Node> nodes;
    void validate() const {
        if (!nodes.contains(entry)) throw std::invalid_argument("invalid continuation entry");
        for (const auto& [id,n] : nodes) {
            if (id != n.id) throw std::invalid_argument("continuation map key mismatch");
            if ((n.transfer == Transfer::Jump || n.transfer == Transfer::Yield || n.transfer == Transfer::BranchNonzero)
                && !nodes.contains(n.next)) throw std::invalid_argument("missing continuation successor");
            if (n.transfer == Transfer::BranchNonzero && !nodes.contains(n.alternative))
                throw std::invalid_argument("missing branch alternative");
        }
    }
};
struct Frame {
    ContinuationId current = 0;
    std::map<Register,std::int64_t> registers;
    bool complete = false;
};
inline Frame initial_frame(const Program& p) { p.validate(); return Frame{p.entry,{},{}}; }
enum class StepStatus { Suspended, Completed, BudgetExhausted };
struct Step { StepStatus status; std::optional<std::int64_t> returned; std::size_t transfers = 0; };
inline std::int64_t checked_add(std::int64_t a,std::int64_t b) {
    const auto max=std::numeric_limits<std::int64_t>::max();
    const auto min=std::numeric_limits<std::int64_t>::min();
    if ((b>0 && a>max-b) || (b<0 && a<min-b)) throw std::overflow_error("continuation addition overflow");
    return a+b;
}
inline std::int64_t checked_sub(std::int64_t a,std::int64_t b) {
    const auto max=std::numeric_limits<std::int64_t>::max();
    const auto min=std::numeric_limits<std::int64_t>::min();
    if ((b<0 && a>max+b) || (b>0 && a<min+b)) throw std::overflow_error("continuation subtraction overflow");
    return a-b;
}
inline Step run(const Program& p, Frame& frame, std::size_t budget) {
    p.validate();
    if (frame.complete) throw std::logic_error("resuming completed frame");
    std::size_t count=0;
    while (count<budget) {
        auto it=p.nodes.find(frame.current);
        if (it==p.nodes.end()) throw std::invalid_argument("frame continuation does not exist");
        const auto& node=it->second;
        auto next_registers = frame.registers;
        auto read=[&](Register name) -> std::int64_t {
            auto value=next_registers.find(name);
            if (value==next_registers.end()) throw std::invalid_argument("uninitialized continuation register");
            return value->second;
        };
        for(const auto& op:node.operations) {
            std::int64_t value=0;
            switch(op.kind) {
                case Arithmetic::Constant:value=op.constant;break;
                case Arithmetic::Copy:value=read(op.left);break;
                case Arithmetic::Add:value=checked_add(read(op.left),read(op.right));break;
                case Arithmetic::Subtract:value=checked_sub(read(op.left),read(op.right));break;
            }
            next_registers[op.output]=value;
        }
        // Perform the transfer only after all computations and required
        // operands have been validated, preserving the frame on failure.
        std::optional<std::int64_t> transfer_value;
        if (node.transfer==Transfer::BranchNonzero || node.transfer==Transfer::Return)
            transfer_value=read(node.condition_or_value);
        frame.registers=std::move(next_registers);
        ++count;
        switch(node.transfer) {
            case Transfer::Jump:frame.current=node.next;break;
            case Transfer::BranchNonzero:frame.current=*transfer_value?node.next:node.alternative;break;
            case Transfer::Yield:frame.current=node.next;return {StepStatus::Suspended,std::nullopt,count};
            case Transfer::Return: {
                auto result=*transfer_value;
                frame.complete=true;
                return {StepStatus::Completed,result,count};
            }
        }
    }
    return {StepStatus::BudgetExhausted,std::nullopt,count};
}
} // namespace moonstone::plaid::continuum
