#include "../Plaid.hpp"
#include <iostream>
int main() {
    using namespace moonstone::plaid;
    loom::WorkStealingPool pool(2, 32); // Bound queued tasks, not active workers.
    conduit::Channel<int> channel(8);
    int sum = 0;
    auto actor = chorus::Actor<int>::create(pool, [&](int v) { sum += v; }, 8);
    for (int i = 1; i <= 5; ++i)
        channel.send(i);
    for (int i = 0; i < 5; ++i)
        actor->send(*channel.receive());
    actor->close_and_wait();
    std::cout << sum << '\n';
}
