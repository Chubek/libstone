#pragma once

#include "config.hpp"
#include "event_queue.hpp"

#include <map>
#include <memory>
#include <string>

namespace domios {

class LiosRuntime {
public:
    LiosRuntime();
    ~LiosRuntime();
    LiosRuntime(const LiosRuntime&) = delete;
    LiosRuntime& operator=(const LiosRuntime&) = delete;

    bool initialize(const Config& config, EventQueue& events, std::string& error);
    bool load_embedded_program(const std::string& name, const std::string& code);
    bool load_disk_programs(const std::vector<std::filesystem::path>& dirs, EventQueue& events);
    bool has_program(const std::string& name) const;
    std::string run_program(const std::string& name, const std::vector<std::string>& args, EventQueue& events,
                            std::string* error = nullptr);
    bool sandbox_intact() const;
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::map<std::string, std::string> programs_;
};

} // namespace domios
