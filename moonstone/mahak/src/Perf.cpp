#include "mahak/Measurement.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace moonstone::mahak {
inline namespace v1 {
namespace {

struct EventMapping {
    std::string name;
    bool supported = false;
    std::uint32_t type = 0;
    std::uint64_t config = 0;
    std::string note;
};

EventMapping map_event(std::string_view name) {
    EventMapping mapping;
    mapping.name = std::string(name);
#if defined(__linux__)
    mapping.supported = true;
    if (name == "cycles")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_CPU_CYCLES;
    else if (name == "instructions")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_INSTRUCTIONS;
    else if (name == "branches")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    else if (name == "branch-misses")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_BRANCH_MISSES;
    else if (name == "cache-references")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_CACHE_REFERENCES;
    else if (name == "cache-misses")
        mapping.type = PERF_TYPE_HARDWARE, mapping.config = PERF_COUNT_HW_CACHE_MISSES;
    else if (name.rfind("raw:", 0) == 0) {
        mapping.type = PERF_TYPE_RAW;
        try {
            mapping.config = std::stoull(std::string(name.substr(4)), nullptr, 0);
        } catch (...) {
            mapping.supported = false;
            mapping.note = "malformed raw event '" + std::string(name) + "'";
        }
    } else {
        mapping.supported = false;
        mapping.note = "unknown perf event '" + std::string(name) + "'";
    }
#else
    mapping.note = "perf is only supported on Linux";
#endif
    return mapping;
}

int perf_open(std::int64_t pid, const EventMapping &mapping, int group_fd, bool include_kernel) {
#if defined(__linux__)
    perf_event_attr attributes{};
    attributes.type = mapping.type;
    attributes.size = sizeof(attributes);
    attributes.config = mapping.config;
    attributes.disabled = 1;
    attributes.inherit = 1;
    attributes.exclude_kernel = include_kernel ? 0 : 1;
    attributes.exclude_hv = 1;
    attributes.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    const long result = ::syscall(__NR_perf_event_open, &attributes, static_cast<pid_t>(pid), -1,
                                 group_fd, 0);
    return static_cast<int>(result);
#else
    (void)pid;
    (void)mapping;
    (void)group_fd;
    (void)include_kernel;
    return -1;
#endif
}

} // namespace

PerfAvailability probe_perf() {
    PerfAvailability availability;
    const EventMapping cycles = map_event("cycles");
    if (!cycles.supported) {
        availability.reason = cycles.note;
        return availability;
    }
#if defined(__linux__)
    const int fd = perf_open(0, cycles, -1, false);
    if (fd < 0) {
        availability.reason = std::string("perf_event_open failed: ") + std::strerror(errno);
        availability.permission_granted =
            errno != EACCES && errno != EPERM;
        return availability;
    }
    ::close(fd);
    availability.available = true;
    availability.permission_granted = true;
    availability.supported_events = {"cycles",      "instructions",   "branches",
                                     "branch-misses", "cache-references", "cache-misses"};
#else
    availability.reason = cycles.note;
#endif
    return availability;
}

struct PerfSession::Impl {
    std::int64_t pid = 0;
    bool stopped = false;
    struct Handle {
        std::string name;
        int fd = -1;
        bool enabled = false;
        std::string note;
    };
    std::vector<Handle> handles;
};

Result<std::unique_ptr<PerfSession>> PerfSession::attach(std::int64_t pid,
                                                         const PerfRequest &request) {
#if !defined(__linux__)
    (void)pid;
    (void)request;
    Error error;
    error.category = ErrorCategory::Unsupported;
    error.message = "perf counters are unavailable on this platform";
    return error;
#else
    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    int leader = -1;
    for (const auto &name : request.events) {
        const EventMapping mapping = map_event(name);
        Impl::Handle handle;
        handle.name = mapping.name;
        if (!mapping.supported) {
            handle.note = mapping.note;
            impl->handles.push_back(std::move(handle));
            continue;
        }
        const int fd = perf_open(pid, mapping, leader, request.include_kernel);
        if (fd < 0) {
            handle.note = std::string("perf_event_open failed: ") + std::strerror(errno);
            impl->handles.push_back(std::move(handle));
            continue;
        }
        handle.fd = fd;
        handle.enabled = true;
        if (leader < 0) {
            leader = fd;
            if (::ioctl(fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0)
                handle.note = std::string("enable failed: ") + std::strerror(errno);
        }
        impl->handles.push_back(std::move(handle));
    }
    if (impl->handles.empty()) {
        Error error;
        error.category = ErrorCategory::InvalidInput;
        error.message = "no perf events were requested";
        return error;
    }
    bool any_enabled = false;
    for (const auto &handle : impl->handles)
        any_enabled = any_enabled || handle.enabled;
    if (!any_enabled) {
        Error error;
        error.category = ErrorCategory::Unsupported;
        error.message = "no requested perf event could be opened: " + impl->handles.front().note;
        return error;
    }
    return std::unique_ptr<PerfSession>(new PerfSession(std::move(impl)));
#endif
}

PerfSession::PerfSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PerfSession::~PerfSession() {
    if (!impl_)
        return;
    for (auto &handle : impl_->handles) {
        if (handle.fd >= 0) {
            ::close(handle.fd);
            handle.fd = -1;
        }
    }
}

Result<std::vector<PerfCounter>> PerfSession::stop() {
    std::vector<PerfCounter> counters;
    if (!impl_ || impl_->stopped) {
        Error error;
        error.category = ErrorCategory::Internal;
        error.message = "perf session already stopped or invalid";
        return error;
    }
    impl_->stopped = true;
    for (auto &handle : impl_->handles) {
        PerfCounter counter;
        counter.name = handle.name;
        if (handle.fd < 0) {
            counter.enabled = false;
            counter.note = handle.note;
            counters.push_back(std::move(counter));
            continue;
        }
        struct {
            std::uint64_t value;
            std::uint64_t time_enabled;
            std::uint64_t time_running;
        } record{};
        const ssize_t bytes = ::read(handle.fd, &record, sizeof(record));
        if (bytes != static_cast<ssize_t>(sizeof(record))) {
            counter.enabled = false;
            counter.note = std::string("read failed: ") + std::strerror(errno);
        } else {
            counter.enabled = true;
            counter.value = record.value;
            if (record.time_running > 0 && record.time_enabled > record.time_running) {
                counter.scaled = true;
                counter.scale = static_cast<double>(record.time_enabled) /
                                static_cast<double>(record.time_running);
                counter.note = "multiplexed; scale applied by consumers";
            }
            if (!handle.note.empty())
                counter.note = handle.note;
        }
        ::close(handle.fd);
        handle.fd = -1;
        counters.push_back(std::move(counter));
    }
    return counters;
}

} // namespace v1
} // namespace moonstone::mahak
