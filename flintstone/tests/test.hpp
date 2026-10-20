#pragma once
#include "Flintstone.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)
template<class F> void rejects(F&& function, const std::string& message = {}) {
    try { function(); }
    catch (const std::exception& e) {
        if (!message.empty() && std::string(e.what()).find(message) == std::string::npos)
            throw std::runtime_error("expected diagnostic containing '" + message + "', got: " + e.what());
        return;
    }
    throw std::runtime_error("expected rejection: " + message);
}
inline flintstone::Module example(const char* file) {
    std::ifstream input(std::string(FLINTSTONE_SOURCE_DIR) + "/examples/" + file);
    CHECK(input.good());
    std::string source{std::istreambuf_iterator<char>(input), {}};
    return flintstone::parse(source, file);
}
inline std::int64_t run(const flintstone::Module& m, const flintstone::Registry& r, const std::string& function,
                        std::vector<firestone::vm::Value> args = {}) {
    return std::get<std::int64_t>(flintstone::execute(m, r, function, args));
}
