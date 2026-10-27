#include <libregalloc/GreedyRegalloc.hpp>
#include <libregalloc/SSABasedSpilling.hpp>
int main() {
    firestone::ir::SimpleSSAGraph ssa;
    auto block = ssa.cfg().create_block("entry");
    auto parameter = ssa.parameter(firestone::ir::Type::integer());
    ssa.cfg().return_value(block, firestone::ir::Operand::ref(parameter));
    auto problem = firestone::regalloc::from_ssa(ssa);
    auto machine = firestone::regalloc::Machine::uniform(1);
    auto allocation = firestone::regalloc::GreedyRegalloc{}.allocate(problem, machine);
    return firestone::regalloc::verify(problem, machine, allocation).empty() ? 0 : 1;
}
