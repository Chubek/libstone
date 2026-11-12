#include "Prolific.hpp"
#include "Internal.hpp"
#include "core/Core.hpp"

#include <atomic>
#include <chrono>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

namespace moonstone::prolific {
const char *name(Domain domain) {
    switch (domain) {
    case Domain::Compiler:
        return "compiler";
    case Domain::VM:
        return "vm";
    case Domain::Runtime:
        return "runtime";
    }
    return "invalid";
}
const char *name(Kind kind) {
    switch (kind) {
    case Kind::Begin:
        return "begin";
    case Kind::End:
        return "end";
    case Kind::Instant:
        return "instant";
    case Kind::Counter:
        return "counter";
    case Kind::Error:
        return "error";
    case Kind::Allocation:
        return "allocation";
    case Kind::Free:
        return "free";
    }
    return "invalid";
}

struct Recorder::Impl {
    struct Frame {
        std::size_t index;
        std::uint64_t children = 0;
    };
    struct Thread {
        std::uint64_t id;
        std::vector<Frame> stack;
    };
    Limits limits;
    mutable std::mutex mutex;
    Snapshot data;
    std::size_t bytes = 0;
    std::map<std::uint64_t, Thread> threads;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    explicit Impl(Limits l) : limits(l) {
        if (!l.events || !l.bytes || !l.text || !l.threads || !l.depth)
            throw std::invalid_argument("Prolific limits must be nonzero");
        if (l.events > 1000000 || l.text > 1024 * 1024)
            throw std::invalid_argument(
                "Prolific v1 supports at most 1,000,000 events and 1 MiB text fields");
        data.created_unix_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
    }
    Code reject(Code code) {
        detail::increment(data.rejected);
        return code;
    }
    static std::uint64_t thread_token() {
        // Unlike std::thread::id, this identity is not reused after thread exit.
        static std::atomic<std::uint64_t> next{1};
        thread_local const auto token = next.fetch_add(1, std::memory_order_relaxed);
        return token;
    }
    Thread *thread() {
        const auto token = thread_token();
        auto found = threads.find(token);
        if (found != threads.end())
            return &found->second;
        if (threads.size() == limits.threads)
            return nullptr;
        return &threads.emplace(token, Thread{threads.size() + 1, {}}).first->second;
    }
    Code admission(Domain domain, Kind kind, std::string_view label, std::string_view detail_text,
                   const Location &location, std::size_t &size) const {
        if (!detail::valid(domain) || !detail::valid(kind) || label.empty())
            return Code::InvalidInput;
        size = sizeof(Event);
        const std::string_view fields[] = {label, detail_text, location.file, location.ir_id};
        for (auto text : fields) {
            if (text.size() > limits.text || size > limits.bytes ||
                text.size() > limits.bytes - size)
                return Code::Exhausted;
            size += text.size();
        }
        if (data.events.size() >= limits.events || size > limits.bytes - bytes)
            return Code::Exhausted;
        for (auto text : fields)
            if (!detail::valid_utf8(text))
                return Code::InvalidInput;
        return Code::Ok;
    }
    void stamp(Event &event, const Thread &thread) const {
        event.sequence = data.events.size() + 1;
        event.thread = thread.id;
        event.timestamp_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
    }
};

Recorder::Recorder(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
Recorder::~Recorder() = default;

BeginResult Recorder::begin(Domain domain, std::string_view label, Location location) {
    std::lock_guard lock(impl_->mutex);
    try {
        std::size_t bytes = 0;
        auto code = impl_->admission(domain, Kind::Begin, label, {}, location, bytes);
        if (code != Code::Ok)
            return {impl_->reject(code), 0};
        auto *thread = impl_->thread();
        if (!thread || thread->stack.size() >= impl_->limits.depth)
            return {impl_->reject(Code::Exhausted), 0};
        Event event;
        event.domain = domain;
        event.kind = Kind::Begin;
        event.name = label;
        event.location = std::move(location);
        impl_->stamp(event, *thread);
        event.span = event.sequence;
        event.parent =
            thread->stack.empty() ? 0 : impl_->data.events[thread->stack.back().index].span;
        const auto span = event.span;
        thread->stack.push_back({impl_->data.events.size(), 0});
        try {
            impl_->data.events.push_back(std::move(event));
        } catch (...) {
            thread->stack.pop_back();
            throw;
        }
        impl_->bytes += bytes;
        ++impl_->data.open_spans;
        return {Code::Ok, span};
    } catch (...) {
        impl_->reject(Code::Exhausted);
        throw;
    }
}

Code Recorder::end(std::uint64_t span) {
    std::lock_guard lock(impl_->mutex);
    try {
        const auto found = impl_->threads.find(Impl::thread_token());
        if (!span || found == impl_->threads.end() || found->second.stack.empty())
            return impl_->reject(Code::InvalidInput);
        auto &thread = found->second;
        const auto frame = thread.stack.back();
        if (impl_->data.events[frame.index].span != span)
            return impl_->reject(Code::InvalidInput);
        const auto &original = impl_->data.events[frame.index];
        std::size_t bytes = 0;
        auto code = impl_->admission(original.domain, Kind::End, original.name, original.detail,
                                     original.location, bytes);
        if (code != Code::Ok)
            return impl_->reject(code);
        Event event = original;
        const auto begin = event.timestamp_ns;
        event.kind = Kind::End;
        impl_->stamp(event, thread);
        event.duration_ns = event.timestamp_ns - begin;
        event.self_ns = event.duration_ns - frame.children;
        const auto duration = event.duration_ns;
        impl_->data.events.push_back(std::move(event));
        impl_->bytes += bytes;
        thread.stack.pop_back();
        if (!thread.stack.empty())
            thread.stack.back().children += duration;
        --impl_->data.open_spans;
        return Code::Ok;
    } catch (...) {
        impl_->reject(Code::Exhausted);
        throw;
    }
}

Code Recorder::emit(Domain domain, Kind kind, std::string_view label, std::int64_t value,
                    std::string_view text, Location location) {
    std::lock_guard lock(impl_->mutex);
    try {
        if (kind == Kind::Begin || kind == Kind::End)
            return impl_->reject(Code::InvalidInput);
        std::size_t bytes = 0;
        auto code = impl_->admission(domain, kind, label, text, location, bytes);
        if (code != Code::Ok)
            return impl_->reject(code);
        auto *thread = impl_->thread();
        if (!thread)
            return impl_->reject(Code::Exhausted);
        Event event;
        event.domain = domain;
        event.kind = kind;
        event.name = label;
        event.detail = text;
        event.value = value;
        event.location = std::move(location);
        impl_->stamp(event, *thread);
        event.parent =
            thread->stack.empty() ? 0 : impl_->data.events[thread->stack.back().index].span;
        impl_->data.events.push_back(std::move(event));
        impl_->bytes += bytes;
        return Code::Ok;
    } catch (...) {
        impl_->reject(Code::Exhausted);
        throw;
    }
}

Snapshot Recorder::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->data;
}
void Recorder::record_loss() {
    std::lock_guard lock(impl_->mutex);
    detail::increment(impl_->data.rejected);
}

Scope::Scope(Recorder &recorder, Domain domain, std::string_view label, Location location)
    : recorder_(recorder), result_(recorder.begin(domain, label, std::move(location))) {}
Scope::~Scope() noexcept {
    if (result_.code == Code::Ok) {
        try {
            recorder_.end(result_.span);
        } catch (...) { /* recorder already counted loss */
        }
    }
}

std::string Report::json() const {
    detail::validate_report(*this);
    auto quote = [](std::string_view s) { return '"' + moonstone::json_escape(s) + '"'; };
    struct Timing {
        std::uint64_t count = 0, total = 0, self = 0, min = UINT64_MAX, max = 0;
        bool overflow = false;
    };
    std::map<std::pair<Domain, std::string>, Timing> timings;
    struct Samples {
        std::uint64_t count = 0;
        std::int64_t sum = 0, min = INT64_MAX, max = INT64_MIN;
        bool overflow = false;
    };
    std::map<std::tuple<Domain, Kind, std::string>, Samples> samples;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"schema\":\"moonstone.prolific/1\",\"created_unix_ns\":" << data.created_unix_ns
        << ",\"complete\":" << (data.complete() ? "true" : "false")
        << ",\"rejected\":" << data.rejected << ",\"open_spans\":" << data.open_spans
        << ",\"analysis\":{\"status\":" << quote(analysis_status)
        << ",\"error\":" << quote(analysis_error) << "},\"events\":[";
    bool comma = false;
    for (const auto &e : data.events) {
        if (std::exchange(comma, true))
            out << ',';
        out << "{\"sequence\":" << e.sequence << ",\"timestamp_ns\":" << e.timestamp_ns
            << ",\"thread\":" << e.thread << ",\"span\":" << e.span << ",\"parent\":" << e.parent
            << ",\"domain\":" << quote(name(e.domain)) << ",\"kind\":" << quote(name(e.kind))
            << ",\"name\":" << quote(e.name) << ",\"detail\":" << quote(e.detail)
            << ",\"value\":" << e.value << ",\"duration_ns\":" << e.duration_ns
            << ",\"self_ns\":" << e.self_ns << ",\"location\":{\"file\":" << quote(e.location.file)
            << ",\"ir_id\":" << quote(e.location.ir_id) << ",\"line\":" << e.location.line
            << ",\"column\":" << e.location.column << "}}";
        if (e.kind == Kind::End) {
            auto &t = timings[{e.domain, e.name}];
            ++t.count;
            if (!detail::add(t.total, e.duration_ns))
                t.overflow = true;
            if (!detail::add(t.self, e.self_ns))
                t.overflow = true;
            t.min = std::min(t.min, e.duration_ns);
            t.max = std::max(t.max, e.duration_ns);
        } else if (e.kind == Kind::Counter || e.kind == Kind::Allocation || e.kind == Kind::Free) {
            auto &s = samples[{e.domain, e.kind, e.name}];
            ++s.count;
            s.min = std::min(s.min, e.value);
            s.max = std::max(s.max, e.value);
            if (!s.overflow) {
                if ((e.value > 0 && s.sum > INT64_MAX - e.value) ||
                    (e.value < 0 && s.sum < INT64_MIN - e.value))
                    s.overflow = true;
                else
                    s.sum += e.value;
            }
        }
    }
    out << "],\"timings\":[";
    comma = false;
    for (const auto &[key, t] : timings) {
        if (std::exchange(comma, true))
            out << ',';
        out << "{\"domain\":" << quote(name(key.first)) << ",\"name\":" << quote(key.second)
            << ",\"completed_spans\":" << t.count << ",\"inclusive_ns\":" << t.total
            << ",\"exclusive_ns\":" << t.self << ",\"min_ns\":" << t.min << ",\"max_ns\":" << t.max
            << ",\"overflow\":" << (t.overflow ? "true" : "false") << '}';
    }
    out << "],\"samples\":[";
    comma = false;
    for (const auto &[key, s] : samples) {
        if (std::exchange(comma, true))
            out << ',';
        out << "{\"domain\":" << quote(name(std::get<0>(key)))
            << ",\"kind\":" << quote(name(std::get<1>(key)))
            << ",\"name\":" << quote(std::get<2>(key)) << ",\"count\":" << s.count << ",\"sum\":";
        if (s.overflow)
            out << "null";
        else
            out << s.sum;
        out << ",\"min\":" << s.min << ",\"max\":" << s.max
            << ",\"overflow\":" << (s.overflow ? "true" : "false") << '}';
    }
    out << "],\"findings\":[";
    comma = false;
    for (const auto &f : findings) {
        if (std::exchange(comma, true))
            out << ',';
        out << "{\"severity\":" << quote(f.severity) << ",\"code\":" << quote(f.code)
            << ",\"message\":" << quote(f.message) << ",\"event_sequence\":" << f.event_sequence
            << ",\"evidence\":\"heuristic\"}";
    }
    out << "]}";
    return out.str();
}
} // namespace moonstone::prolific
