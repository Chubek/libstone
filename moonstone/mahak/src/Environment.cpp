#include "mahak/Measurement.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <utility>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include "mahak/Adapter.hpp"

namespace moonstone::mahak {
inline namespace v1 {
namespace {

std::string read_file(std::string_view path) {
    std::ifstream stream{std::string(path), std::ios::binary};
    if (!stream)
        return {};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string first_line(const std::string &text) {
    const std::size_t newline = text.find('\n');
    return text.substr(0, newline == std::string::npos ? text.size() : newline);
}

std::string os_release_name() {
    const std::string text = read_file("/etc/os-release");
    std::istringstream stream(text);
    std::string line;
    std::string fallback;
    while (std::getline(stream, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string value = line.substr(12);
            if (!value.empty() && value.front() == '"')
                value.erase(value.begin());
            if (!value.empty() && value.back() == '"')
                value.pop_back();
            return value;
        }
        if (line.rfind("NAME=", 0) == 0 && fallback.empty())
            fallback = line.substr(5);
    }
    return fallback;
}

void parse_cpuinfo(EnvironmentInfo &environment) {
    const std::string text = read_file("/proc/cpuinfo");
    if (text.empty())
        return;
    std::istringstream stream(text);
    std::string line;
    std::vector<std::string> physical;
    std::vector<std::string> cores;
    while (std::getline(stream, line)) {
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos)
            continue;
        std::string key = line.substr(0, separator);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        std::string value = line.substr(separator + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        if ((key == "model name" || key == "Processor" || key == "cpu model") &&
            environment.cpu_model.empty())
            environment.cpu_model = value;
        if (key == "physical id" || key == "package id")
            physical.push_back(value);
        if (key == "core id")
            cores.push_back(value);
    }
    if (!physical.empty() && physical.size() == cores.size()) {
        std::vector<std::string> unique;
        for (std::size_t index = 0; index < physical.size(); ++index) {
            const std::string pair = physical[index] + "/" + cores[index];
            bool seen = false;
            for (const auto &candidate : unique)
                seen = seen || candidate == pair;
            if (!seen)
                unique.push_back(pair);
        }
        environment.physical_cpus = static_cast<std::uint32_t>(unique.size());
    }
}

std::string affinity_mask() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0)
        return {};
    std::string mask;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &set))
            continue;
        if (!mask.empty())
            mask.push_back(',');
        mask += std::to_string(cpu);
    }
    return mask;
#else
    return {};
#endif
}

} // namespace

EnvironmentInfo probe_environment() {
    EnvironmentInfo environment;
    utsname system{};
    if (::uname(&system) == 0) {
        environment.os = system.sysname;
        environment.kernel = std::string(system.release) + " " + system.version;
        environment.architecture = system.machine;
    }
    const std::string distribution = os_release_name();
    if (!distribution.empty() && !environment.os.empty())
        environment.os += " (" + distribution + ")";
    const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0)
        environment.logical_cpus = static_cast<std::uint32_t>(cpus);

    parse_cpuinfo(environment);
    environment.affinity = affinity_mask();
    environment.cpu_frequency_policy =
        first_line(read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"));

    const char *compiler = std::getenv("CC");
    const std::string compiler_name = compiler != nullptr && *compiler != '\0' ? compiler : "cc";
    ToolProbe probe = probe_tool(compiler_name);
    if (probe.available) {
        environment.compiler = probe.path;
        environment.compiler_version = probe.version;
    } else {
        environment.extra["compiler_probe"] = probe.diagnostics;
    }
    environment.extra["clock"] = "CLOCK_MONOTONIC";
    return environment;
}

} // namespace v1
} // namespace moonstone::mahak
