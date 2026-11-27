#include <TTyUtils/TTyUtils-Completion.hpp>
#include <cassert>
#include <future>
#include <memory>

int main() {
    ttyutils::completion::Engine engine;
    engine.add(std::make_unique<ttyutils::completion::WordSource>(
        std::vector<std::string>{"alpha", "beta"}));
    const auto result = engine.query("al");
    assert(result.size() == 1);
    assert(result.front().text == "alpha");

    engine.add(std::make_unique<ttyutils::completion::WordSource>(
        std::vector<std::string>{"alpha", "alphabet", "alpaca"}));
    const auto page = engine.query_for("alp", std::chrono::seconds{2}, 1, 1);
    assert(page.size() == 1 && page.front().text == "alpaca");
    assert(ttyutils::completion::Engine::common_prefix({{"café"}, {"cafê"}}) == "caf");

    struct Gate {
        std::promise<void> entered;
        std::promise<void> release;
        std::promise<void> finished;
    };
    const auto gate = std::make_shared<Gate>();
    auto entered = gate->entered.get_future();
    auto finished = gate->finished.get_future();
    auto release = gate->release.get_future().share();
    ttyutils::completion::Engine bounded;
    bounded.add(std::make_unique<ttyutils::completion::CallbackSource>([gate, release](auto) {
        gate->entered.set_value();
        release.wait();
        gate->finished.set_value();
        return std::vector<ttyutils::completion::Candidate>{{"slow"}};
    }));
    assert(bounded.query_for("s", std::chrono::milliseconds{10}).empty());
    assert(entered.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    assert(bounded.query("s").empty());
    gate->release.set_value();
    assert(finished.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    return 0;
}
