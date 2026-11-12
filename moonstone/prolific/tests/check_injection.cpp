#include "prolific/Cache.hpp"
#include <iostream>

int main(int argc, char **argv) {
    using namespace moonstone::prolific;
    try {
        if (argc != 2)
            return 1;
        Cache cache(argv[1], true);
        auto report = cache.load("injected");
        if (!report || !report->data.complete() || report->data.events.size() != 60)
            return 2;
        for (const auto &e : report->data.events)
            if (e.domain != Domain::Runtime || e.location.ir_id.empty() ||
                !e.location.file.empty() || e.location.line != 0)
                return 3;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 4;
    }
}
