#include "prolific/Cache.hpp"
#include "prolific/LuaProfiler.hpp"
#include "prolific/Speculation.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char **argv) {
    using namespace moonstone::prolific;
    try {
        if (argc < 3)
            throw std::invalid_argument("usage: prolific list CACHE | report CACHE RUN | analyze "
                                        "CACHE RUN SCRIPT [OUTPUT_RUN] | speculate CACHE RUN");
        const std::string command = argv[1];
        if (command == "list" && argc == 3) {
            Cache cache(argv[2], true);
            for (const auto &run : cache.runs())
                std::cout << run << '\n';
        } else if (command == "report" && argc == 4) {
            Cache cache(argv[2], true);
            auto json = cache.json(argv[3]);
            if (!json)
                throw std::runtime_error("Prolific run not found");
            std::cout << *json << '\n';
        } else if (command == "speculate" && argc == 4) {
            Cache cache(argv[2], true);
            auto report = cache.load(argv[3]);
            if (!report)
                throw std::runtime_error("Prolific run not found");
            std::cout << speculate(report->data);
        } else if (command == "analyze" && (argc == 5 || argc == 6)) {
            Cache cache(argv[2], argc != 6);
            auto report = cache.load(argv[3]);
            if (!report)
                throw std::runtime_error("Prolific run not found");
            std::ifstream input(argv[4], std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot open Prolific script");
            std::string script;
            char buffer[4096];
            while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
                script.append(buffer, static_cast<std::size_t>(input.gcount()));
                if (script.size() > ScriptLimits{}.source_bytes)
                    throw std::runtime_error("Prolific script is too large");
            }
            if (input.bad())
                throw std::runtime_error("cannot read Prolific script");
            auto analyzed = analyze(std::move(report->data), script);
            if (argc == 6)
                cache.save(argv[5], analyzed);
            std::cout << analyzed.json() << '\n';
            if (analyzed.analysis_status == "failed")
                return 2;
        } else
            throw std::invalid_argument("invalid Prolific command or argument count");
        return std::cout ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "prolific: " << error.what() << '\n';
        return 1;
    }
}
