#include "Internal.hpp"
#include "Prolific.hpp"

#include <bit>
#include <map>

namespace moonstone::prolific {
namespace {
constexpr std::string_view magic{"PROLIFIC\1", 9};
constexpr std::size_t wire_limit = 128 * 1024 * 1024;
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("invalid or unsupported Prolific snapshot");
}
void text(std::string_view s) { require(s.size() <= 1024 * 1024 && detail::valid_utf8(s)); }
bool same_location(const Location &a, const Location &b) {
    return a.file == b.file && a.ir_id == b.ir_id && a.line == b.line && a.column == b.column;
}
void validate(const Report &report) {
    require(report.data.events.size() <= 1000000 && report.findings.size() <= 100000);
    require(report.analysis_status == "not_requested" || report.analysis_status == "completed" ||
            report.analysis_status == "failed");
    require((report.analysis_status == "failed") == !report.analysis_error.empty());
    require(report.findings.empty() || report.analysis_status == "completed");
    text(report.analysis_error);
    struct Frame {
        const Event *event;
        std::uint64_t children = 0;
    };
    std::map<std::uint64_t, std::vector<Frame>> stacks;
    std::uint64_t sequence = 0, timestamp = 0, open = 0;
    for (const auto &e : report.data.events) {
        require(e.sequence == ++sequence && e.timestamp_ns >= timestamp && e.thread != 0 &&
                detail::valid(e.domain) && detail::valid(e.kind) && !e.name.empty());
        timestamp = e.timestamp_ns;
        text(e.name);
        text(e.detail);
        text(e.location.file);
        text(e.location.ir_id);
        auto &stack = stacks[e.thread];
        if (e.kind == Kind::End) {
            require(!stack.empty());
            const auto frame = stack.back();
            const auto &begin = *frame.event;
            require(e.span == begin.span && e.parent == begin.parent && e.domain == begin.domain &&
                    e.name == begin.name && same_location(e.location, begin.location) &&
                    e.value == 0 && e.detail.empty() &&
                    e.duration_ns == e.timestamp_ns - begin.timestamp_ns &&
                    frame.children <= e.duration_ns && e.self_ns == e.duration_ns - frame.children);
            stack.pop_back();
            if (!stack.empty())
                require(detail::add(stack.back().children, e.duration_ns));
            --open;
        } else {
            require(e.parent == (stack.empty() ? 0 : stack.back().event->span));
            require(e.duration_ns == 0 && e.self_ns == 0);
            if (e.kind == Kind::Begin) {
                require(e.span == e.sequence && e.value == 0 && e.detail.empty());
                stack.push_back({&e, 0});
                ++open;
            } else
                require(e.span == 0);
        }
    }
    require(open == report.data.open_spans);
    for (const auto &f : report.findings) {
        text(f.severity);
        text(f.code);
        text(f.message);
        require(detail::severity(f.severity) && !f.code.empty() && !f.message.empty() &&
                f.event_sequence <= sequence);
    }
}
struct Writer {
    std::string bytes{magic};
    void number(std::uint64_t value) {
        require(bytes.size() <= wire_limit - 8);
        for (unsigned i = 0; i != 8; ++i)
            bytes.push_back(static_cast<char>(value >> (i * 8)));
    }
    void string(std::string_view value) {
        require(value.size() <= wire_limit - 8 && bytes.size() <= wire_limit - 8 - value.size());
        number(value.size());
        bytes.append(value);
    }
};
struct Reader {
    std::string_view bytes;
    std::uint64_t number() {
        require(bytes.size() >= 8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
        bytes.remove_prefix(8);
        return value;
    }
    std::string string() {
        const auto size = number();
        require(size <= 1024 * 1024 && size <= bytes.size());
        std::string value(bytes.substr(0, static_cast<std::size_t>(size)));
        bytes.remove_prefix(static_cast<std::size_t>(size));
        return value;
    }
};
} // namespace

void detail::validate_report(const Report &report) { validate(report); }

std::string encode(const Report &report) {
    validate(report);
    Writer out;
    out.number(report.data.created_unix_ns);
    out.number(report.data.rejected);
    out.number(report.data.open_spans);
    out.string(report.analysis_status);
    out.string(report.analysis_error);
    out.number(report.data.events.size());
    for (const auto &e : report.data.events) {
        out.number(e.sequence);
        out.number(e.timestamp_ns);
        out.number(e.thread);
        out.number(e.span);
        out.number(e.parent);
        out.number(static_cast<unsigned>(e.domain));
        out.number(static_cast<unsigned>(e.kind));
        out.number(std::bit_cast<std::uint64_t>(e.value));
        out.number(e.duration_ns);
        out.number(e.self_ns);
        out.string(e.name);
        out.string(e.detail);
        out.string(e.location.file);
        out.string(e.location.ir_id);
        out.number(e.location.line);
        out.number(e.location.column);
    }
    out.number(report.findings.size());
    for (const auto &f : report.findings) {
        out.string(f.severity);
        out.string(f.code);
        out.string(f.message);
        out.number(f.event_sequence);
    }
    return std::move(out.bytes);
}

Report decode(std::string_view bytes, std::size_t max_bytes) {
    require(bytes.size() <= max_bytes && bytes.substr(0, magic.size()) == magic);
    Reader in{bytes.substr(magic.size())};
    Report report;
    report.data.created_unix_ns = in.number();
    report.data.rejected = in.number();
    report.data.open_spans = in.number();
    report.analysis_status = in.string();
    report.analysis_error = in.string();
    auto count = in.number();
    require(count <= 1000000 && count <= in.bytes.size() / 128);
    report.data.events.reserve(static_cast<std::size_t>(count));
    while (count--) {
        Event e;
        e.sequence = in.number();
        e.timestamp_ns = in.number();
        e.thread = in.number();
        e.span = in.number();
        e.parent = in.number();
        const auto domain = in.number(), kind = in.number();
        require(domain <= 2 && kind <= 6);
        e.domain = static_cast<Domain>(domain);
        e.kind = static_cast<Kind>(kind);
        e.value = std::bit_cast<std::int64_t>(in.number());
        e.duration_ns = in.number();
        e.self_ns = in.number();
        e.name = in.string();
        e.detail = in.string();
        e.location.file = in.string();
        e.location.ir_id = in.string();
        const auto line = in.number(), column = in.number();
        require(line <= UINT32_MAX && column <= UINT32_MAX);
        e.location.line = static_cast<std::uint32_t>(line);
        e.location.column = static_cast<std::uint32_t>(column);
        report.data.events.push_back(std::move(e));
    }
    count = in.number();
    require(count <= 100000 && count <= in.bytes.size() / 32);
    report.findings.reserve(static_cast<std::size_t>(count));
    while (count--) {
        Finding f;
        f.severity = in.string();
        f.code = in.string();
        f.message = in.string();
        f.event_sequence = in.number();
        report.findings.push_back(std::move(f));
    }
    require(in.bytes.empty());
    validate(report);
    return report;
}
} // namespace moonstone::prolific
