#include <libisel/BURS.hpp>
#include <libisel/Verify.hpp>
#include <libisched/RegisterAllocation.hpp>
#include <libregalloc/LinearScanRegalloc.hpp>
#include <libisched/ListSched.hpp>
#include <libisched/IntegerProgSched.hpp>
int main() {
    firestone::isel::Graph graph;
    auto root = graph.add("literal");
    firestone::isel::Rules rules;
    firestone::isel::Rule rule;
    rule.name = "literal"; rule.pattern = firestone::rewrite::Term("literal"); rule.instruction = "MOV";
    rules.add(rule);
    auto selection = firestone::isel::BURS{}.select(graph, rules, {root});
    auto operations = firestone::isched::from_selection(selection);
    auto schedule = firestone::isched::ListSched{}.schedule(operations);
    if (!firestone::isel::verify(graph, rules, {root}, selection).empty()) return 1;
    auto input = firestone::isched::allocation_problem(selection, schedule);
    auto machine = firestone::regalloc::Machine::uniform(1);
    auto allocation = firestone::regalloc::LinearScanRegalloc{}.allocate(input.problem, machine);
    if (!firestone::regalloc::verify(input.problem, machine, allocation).empty()) return 1;
    return firestone::isched::verify(operations, {}, schedule).empty() && schedule.makespan == 1 ? 0 : 1;
}
