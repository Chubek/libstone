#include "shell.hpp"

#include <sstream>

namespace domios {
namespace {

std::vector<std::string> split(std::string line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) {
        out.push_back(word);
    }
    return out;
}

} // namespace

std::string execute_shell_line(LiosRuntime& lua, const std::string& line, EventQueue& events) {
    const auto words = split(line);
    if (words.empty()) {
        return {};
    }
    const auto& cmd = words.front();
    if (cmd == "help") {
        return "C++ built-ins shadow Lua programs: help, exit, info.";
    }
    if (cmd == "exit") {
        return "exit";
    }
    if (cmd == "info") {
        return lua.sandbox_intact() ? "sandbox ok" : "sandbox broken";
    }
    if (lua.has_program(cmd)) {
        return lua.run_program(cmd, std::vector<std::string>(words.begin() + 1, words.end()), events);
    }
    return "unknown command";
}

} // namespace domios
