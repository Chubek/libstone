#include "mahak/coordination/Broker.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace moonstone::mahak::coordination;

namespace {

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            if (!current.empty())
                parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

} // namespace

int main(int argc, char **argv) {
    BrokerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char *name) -> std::string {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++index];
        };
        if (argument == "--session")
            options.session = value("--session");
        else if (argument == "--workers")
            options.workers = split(value("--workers"));
        else if (argument == "--blotter-root")
            options.blotter_root = value("--blotter-root");
        else if (argument == "--heartbeat-ms")
            options.heartbeat_timeout_ms = static_cast<std::uint32_t>(std::stoul(value("--heartbeat-ms")));
        else if (argument == "--request-timeout-ms")
            options.request_timeout_ms = static_cast<std::uint32_t>(std::stoul(value("--request-timeout-ms")));
        else if (argument == "--queue-depth")
            options.queue_depth = static_cast<std::uint32_t>(std::stoul(value("--queue-depth")));
        else if (argument == "--verbose")
            options.verbose = true;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: mahak-coord-broker_main --session ID --workers 1,2,3,..."
                         " [--blotter-root DIR] [--heartbeat-ms N] [--verbose]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 2;
        }
    }
    if (options.session.empty() || options.workers.size() < 2) {
        std::cerr << "--session and at least two --workers are required\n";
        return 2;
    }
    auto result = run_broker(options);
    if (!result) {
        std::cerr << result.error().message << "\n";
        return 1;
    }
    std::cout << "received=" << result.value().received
              << " routed=" << result.value().routed
              << " duplicates=" << result.value().duplicates
              << " rejected=" << result.value().rejected
              << " backpressure=" << result.value().backpressure
              << " timeouts=" << result.value().timeouts
              << " heartbeat_failures=" << result.value().heartbeat_failures
              << " errors_sent=" << result.value().errors_sent << "\n";
    return 0;
}
