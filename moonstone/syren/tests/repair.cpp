#include <cassert>
#include <syren/Repair.hpp>

using namespace moonstone;
using namespace moonstone::syren;

int main() {
    RepairCandidate candidate{"edit", "replace a", {{0, 1, "x"}}, 1, RepairStatus::Candidate, {}};
    for (auto status : {Status::Proven, Status::Refuted, Status::Unknown, Status::BudgetExhausted,
                        Status::InvalidInput}) {
        auto checked = verify_repair(candidate, "a", [=](std::string_view source) {
            assert(source == "x");
            return ProofResult{status, "independent host check", {}, 1};
        });
        assert((checked.status == RepairStatus::Validated) == (status == Status::Proven));
        assert(checked.reason == "independent host check");
        if (status == Status::Unknown || status == Status::BudgetExhausted) {
            assert(checked.status == RepairStatus::Inconclusive);
        }
    }
    assert(verify_repair(candidate, "a", {}).status == RepairStatus::Rejected);
    bool called = false;
    auto validator = [&](std::string_view) {
        called = true;
        return ProofResult{};
    };
    candidate.edits = {{0, 2, "x"}};
    assert(verify_repair(candidate, "a", validator).status == RepairStatus::Rejected && !called);
    candidate.edits = {{0, 0, "x"}, {0, 0, "y"}};
    assert(verify_repair(candidate, "a", validator).status == RepairStatus::Rejected && !called);
    candidate.edits.clear();
    candidate.cost = -1;
    assert(verify_repair(candidate, "a", validator).status == RepairStatus::Rejected && !called);

    std::vector<RepairCandidate> repairs;
    for (auto status : {RepairStatus::Rejected, RepairStatus::Validated, RepairStatus::Inconclusive,
                        RepairStatus::Candidate}) {
        for (auto cost : {3, 1, 2}) {
            repairs.push_back({"b", "", {}, cost, status, "unchanged"});
            repairs.push_back({"a", "", {}, cost, status, "unchanged"});
        }
    }
    // Exercise all cyclic input orderings; status equivalence must not conflict
    // with cost ordering (the previous comparator violated strict weak ordering).
    for (std::size_t rotation = 0; rotation < repairs.size(); ++rotation) {
        auto ranked = rank_repairs(repairs);
        for (std::size_t group = 0; group < 4; ++group) {
            auto expected = std::vector{RepairStatus::Validated, RepairStatus::Candidate,
                                        RepairStatus::Inconclusive, RepairStatus::Rejected}[group];
            for (std::size_t index = 0; index < 6; ++index) {
                const auto &repair = ranked[group * 6 + index];
                assert(repair.status == expected &&
                       repair.cost == static_cast<std::int64_t>(index / 2 + 1));
                assert(repair.id == (index % 2 == 0 ? "a" : "b") && repair.reason == "unchanged");
            }
        }
        std::rotate(repairs.begin(), repairs.begin() + 1, repairs.end());
    }
}
