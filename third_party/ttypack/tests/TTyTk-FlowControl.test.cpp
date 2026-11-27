#include <TTyTk/TTyTk-FlowControl.hpp>

#include <cassert>
#include <limits>

int main() {
    ttytk::flow::Throttle throttle{4};
    assert(throttle.wait_for_capacity(4));
    assert(!throttle.wait_for_capacity(1));
    throttle.acknowledge(4);
    assert(throttle.wait_for_capacity(1));
    throttle.acknowledge(std::numeric_limits<std::size_t>::max());
    assert(throttle.wait_for_capacity(4));

    ttytk::flow::Backpressure pressure{3};
    pressure.add(3);
    assert(pressure.active());
    pressure.consume(3);
    assert(!pressure.active());
    pressure.add(std::numeric_limits<std::size_t>::max());
    pressure.add(1);
    assert(pressure.active());
    pressure.consume(std::numeric_limits<std::size_t>::max() - 3);
    assert(pressure.active());
    pressure.consume(1);
    assert(!pressure.active());

    ttytk::flow::XonXoff xon_xoff{};
    xon_xoff.feed(0x13);
    assert(xon_xoff.paused());
    xon_xoff.feed('A');
    assert(xon_xoff.paused());
    xon_xoff.feed(0x11);
    assert(!xon_xoff.paused());
}
