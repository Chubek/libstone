#include <TTyUtils/TTyUtils-Progress.hpp>
#include <cassert>
#include <optional>
using namespace ttyutils::ui;

int main() {
    ProgressBar bar;
    bar.set_total(10);
    bar.advance(5);
    assert(bar.fraction() == 0.5 && bar.line(3) == "█▌ ");
    bar.set_total(2);
    assert(bar.fraction() == 1 && bar.line(3) == "███");
    bar.set_value(std::nan(""));
    assert(bar.fraction() == 1 && bar.line(3, false) == "===");

    std::optional<MultiProgress::Task> retained;
    {
        MultiProgress progress;
        retained = progress.add("first", 10);
        for (int i = 0; i < 128; ++i) progress.add("other", 30);
        retained->advance(2);
        assert(retained->value() == 2);
        retained->advance(20);
        assert(retained->value() == 10);
    }
    retained->advance(2);
    assert(retained->value() == 0);
    EtaModel eta;
    eta.update(100, std::chrono::seconds{1});
    eta.update(0, std::chrono::seconds{1});
    assert(eta.rate() > 0 && eta.rate() < 100 && eta.remaining(100) > 1);
}
