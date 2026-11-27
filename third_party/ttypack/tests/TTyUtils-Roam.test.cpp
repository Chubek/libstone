#include <TTyUtils/TTyUtils-Roam.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::roam;
    Predictor predictor;
    predictor.echo("a");
    predictor.echo("bc");
    predictor.confirm(2);
    assert(predictor.pending() == "c");
    predictor.confirm(100);
    assert(predictor.pending().empty());
    Client client;
    StateSync state;
    assert(client.state() == nullptr);
    client.attach(state);
    state.push("frame");
    assert(client.state()->frames().front() == "frame");
}
