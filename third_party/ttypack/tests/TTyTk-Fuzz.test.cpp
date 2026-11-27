#include <TTyTk/TTyTk-Fuzz.hpp>

#include <algorithm>
#include <cassert>

int main() {
    using namespace ttytk::fuzz;
    for (std::size_t length = 0; length < 100; ++length) {
        for (std::uint64_t seed = 0; seed < 20; ++seed) {
            const auto sample = random_sequence(length, seed);
            assert(sample.size() == length && sample == random_sequence(length, seed));
        }
    }
    const std::string sample{"sample text"};
    const auto modified = mutate(sample, 42);
    assert(modified.size() == sample.size() && modified == mutate(sample, 42));
    assert(std::mismatch(sample.begin(), sample.end(), modified.begin()).first != sample.end());
    assert(mutate({}).size() == 1);
    ttytk::Automaton automaton;
    automaton.max_sequence_bytes = 20;
    unsigned print{}, csi{}, osc{}, dcs{};
    automaton.on_print = [&](std::string_view) { ++print; };
    automaton.on_csi = [&](std::string_view) { ++csi; };
    automaton.on_osc = [&](std::string_view) { ++osc; };
    automaton.on_dcs = [&](std::string_view) { ++dcs; };
    stress_test(automaton, 1000);
    assert(print > 0 && csi > 0 && osc > 0 && dcs > 0);
}
