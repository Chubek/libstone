#include <libisel/BURS.hpp>
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
    return firestone::isched::verify(operations, {}, schedule).empty() && schedule.makespan == 1 ? 0 : 1;
}
