#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Blotter.hpp"
#include "Mailbox.hpp"
#include "Protocol.hpp"

namespace moonstone::mahak::coordination {

struct BrokerOptions {
    std::string session;
    std::string blotter_root; // empty selects /dev/shm/mahak-<session>
    // When set, WORKERS.json is read from this directory and recipients may be
    // addressed by comrade name; the broker resolves the name to the numeric
    // protocol id before routing. Empty disables named routing.
    std::string roster_root;
    std::vector<std::string> workers; // worker ids expected to register
    std::uint32_t queue_depth = 1;
    std::uint32_t heartbeat_timeout_ms = 5000;
    std::uint32_t request_timeout_ms = 15000;
    std::uint32_t delivery_retries = 3;
    std::uint32_t shutdown_grace_ms = 2000;
    bool verbose = false;
};

struct BrokerStats {
    std::uint64_t received = 0;
    std::uint64_t routed = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t rejected = 0;
    std::uint64_t backpressure = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t heartbeat_failures = 0;
    std::uint64_t errors_sent = 0;
};

// Broker process. It owns the ingress queue, creates the expected worker
// mailboxes, routes messages, deduplicates by (sender,sequence), acknowledges
// delivery, detects unresponsive workers, reports deadlock diagnostics to the
// orchestrator (worker "1"), and unlinks its queues on orderly shutdown.
//
// run() blocks until a shutdown REQUEST/COMPLETE is received or *stop becomes
// true. It never exits by silently abandoning live worker queues.
Result<BrokerStats> run_broker(const BrokerOptions &options, const std::atomic<bool> *stop = nullptr);

} // namespace moonstone::mahak::coordination
