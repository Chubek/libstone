#include "Speculation.hpp"
#include "Internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <tuple>
#include <utility>

namespace moonstone::prolific {
namespace {
constexpr std::string_view event_name = "prolific.profile/1";
constexpr std::array names{"type",           "branch",    "value", "call",
                           "interoperation", "system",    "cost",  "lowering",
                           "allocation",     "exception", "custom"};
constexpr std::size_t max_text = 1024 * 1024;

bool valid(const Observation &o) {
    if (static_cast<unsigned>(o.kind) >= names.size() || o.site.empty() || o.context.empty() ||
        o.dimension.empty())
        return false;
    for (const auto *text : {&o.site, &o.context, &o.dimension, &o.outcome, &o.unit})
        if (text->size() > max_text || !detail::valid_utf8(*text))
            return false;
    if (numeric(o.kind)) {
        return o.outcome.empty() && !o.unit.empty() &&
               ((o.kind != ProfileKind::Cost && o.kind != ProfileKind::Allocation) || o.value >= 0);
    }
    return !o.outcome.empty() && o.unit.empty() && o.value == 0 &&
           (o.kind != ProfileKind::Branch || o.outcome == "true" || o.outcome == "false");
}
void field(std::string &output, std::string_view value) {
    output += std::to_string(value.size());
    output += ':';
    output += value;
}
std::string_view field(std::string_view &input) {
    auto colon = input.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 7 ||
        (colon > 1 && input.front() == '0'))
        throw std::invalid_argument("malformed Prolific profile field length");
    std::size_t size = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + colon, size);
    if (error != std::errc{} || end != input.data() + colon || size > max_text ||
        size > input.size() - colon - 1)
        throw std::invalid_argument("invalid Prolific profile field size");
    input.remove_prefix(colon + 1);
    const auto result = input.substr(0, size);
    input.remove_prefix(size);
    return result;
}
Observation observation(const Event &e) {
    if (e.name != event_name || e.kind != Kind::Instant)
        throw std::invalid_argument("unsupported Prolific profile version or event kind");
    auto bytes = std::string_view(e.detail);
    const auto category = field(bytes);
    Observation o;
    auto it = std::find(names.begin(), names.end(), category);
    if (it == names.end())
        throw std::invalid_argument("unknown Prolific profile category");
    o.kind = static_cast<ProfileKind>(it - names.begin());
    o.site = field(bytes);
    o.context = field(bytes);
    o.dimension = field(bytes);
    o.outcome = field(bytes);
    o.unit = field(bytes);
    o.value = e.value;
    if (!bytes.empty() || !valid(o))
        throw std::invalid_argument("invalid Prolific observation");
    return o;
}

// Escape bytes, not Unicode code points. Fixed-width decimal Lua escapes avoid
// ambiguity with following digits and do not interpret untrusted text as code.
std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c == 127) {
            out += '\\';
            out += static_cast<char>('0' + c / 100);
            out += static_cast<char>('0' + (c / 10) % 10);
            out += static_cast<char>('0' + c % 10);
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
std::string number(std::uint64_t value) { return quoted(std::to_string(value)); }
std::string integer(std::int64_t value) { return quoted(std::to_string(value)); }
struct Output {
    std::string text;
    std::size_t limit, count = 0, count_limit;
    void append(std::string_view value) {
        if (value.size() > limit - text.size())
            throw std::length_error("SpecIR output budget exhausted");
        text += value;
    }
    void instruction(std::string_view op, const std::string &fields) {
        if (count == count_limit)
            throw std::length_error("SpecIR instruction budget exhausted");
        ++count;
        append("  ir.");
        append(op);
        append("{");
        append(fields);
        append("},\n");
    }
};
struct Profile {
    Observation observation;
    Domain domain;
    Location location;
    std::uint64_t count = 0, omitted = 0, first = 0, last = 0;
    std::map<std::string, std::uint64_t> buckets;
    std::int64_t min = INT64_MAX, max = INT64_MIN, sum = 0;
    bool overflow = false;
};
using Key = std::tuple<Domain, ProfileKind, std::string, std::string, std::string, std::string,
                       std::string, std::string, std::uint32_t, std::uint32_t>;
std::string location(const Location &loc) {
    return "{file=" + quoted(loc.file) + ",ir_id=" + quoted(loc.ir_id) +
           ",line=" + std::to_string(loc.line) + ",column=" + std::to_string(loc.column) + '}';
}
struct Rule {
    const char *predicate;
    const char *phase;
    const char *miss;
    const char *advice;
};
Rule rule(ProfileKind kind, bool range) {
    switch (kind) {
    case ProfileKind::Type:
        return {"type_is", "before_use", "generic_path",
                "Consider a type-specialized path; retain the generic path."};
    case ProfileKind::Branch:
        return {"branch_is", "after_condition", "cold_path",
                "Favor the observed branch; preserve evaluation and the other edge."};
    case ProfileKind::Value:
        return {range ? "value_in_range" : "value_is", "before_use", "generic_path",
                "Consider value specialization with checked arithmetic and generic fallback."};
    case ProfileKind::Call:
        return {"callee_is", "before_dispatch", "generic_dispatch",
                "Consider guarded direct dispatch or inlining of the observed target."};
    case ProfileKind::Interoperation:
        return {"interop_contract_is", "before_boundary", "generic_boundary",
                "Consider a specialized boundary adapter after independently checking ABI, "
                "ownership and effects."};
    case ProfileKind::System:
        return {"system_outcome_is", "after_effect", "reprofile",
                "Monitor the common system outcome; do not remove, predict or reorder the "
                "system effect."};
    case ProfileKind::Cost:
        return {"cost_in_range", "after_measurement", "reprofile",
                "Investigate this measured cost range; compare alternative implementations "
                "under the same context and unit."};
    case ProfileKind::Lowering:
        return {"lowering_context_is", "before_lowering", "generic_lowering",
                "Consider the observed lowering choice only after independent target-feature "
                "and semantic legality checks."};
    case ProfileKind::Allocation:
        return {range ? "allocation_in_range" : "allocation_is", "before_allocation",
                "generic_allocator",
                "Consider a size-specialized allocation path while preserving ownership, "
                "alignment and collection contracts."};
    case ProfileKind::Exception:
        return {"exception_is", "after_exception", "generic_handler",
                "Investigate the observed exception pattern; preserve all exceptional paths."};
    case ProfileKind::Custom:
        return {"custom_is", "before_use", "generic_path",
                "Investigate this recurring host-defined observation; define predicate "
                "semantics in the translator."};
    }
    throw std::invalid_argument("unknown speculation kind");
}
} // namespace

const char *name(ProfileKind kind) {
    const auto i = static_cast<unsigned>(kind);
    return i < names.size() ? names[i] : "invalid";
}
bool numeric(ProfileKind kind) {
    return kind == ProfileKind::Value || kind == ProfileKind::Cost ||
           kind == ProfileKind::Allocation;
}
Code observe(Recorder &recorder, Domain domain, const Observation &o, Location loc) {
    if (!valid(o)) {
        recorder.record_loss();
        return Code::InvalidInput;
    }
    std::string bytes;
    try {
        for (std::string_view s : {std::string_view(name(o.kind)), std::string_view(o.site),
                                   std::string_view(o.context), std::string_view(o.dimension),
                                   std::string_view(o.outcome), std::string_view(o.unit)}) {
            const auto overhead = std::to_string(s.size()).size() + 1;
            if (s.size() + overhead > max_text - bytes.size()) {
                recorder.record_loss();
                return Code::Exhausted;
            }
            field(bytes, s);
        }
    } catch (...) {
        recorder.record_loss();
        throw;
    }
    return recorder.emit(domain, Kind::Instant, event_name, o.value, bytes, std::move(loc));
}
std::optional<Observation> profile_observation(const Event &event) {
    if (!event.name.starts_with("prolific.profile/"))
        return std::nullopt;
    return observation(event);
}

std::string speculate(const Snapshot &snapshot, SpeculationPolicy policy) {
    if (!policy.minimum_samples || policy.minimum_samples > 1000000 ||
        policy.dominance_permille < 501 || policy.dominance_permille > 1000 || !policy.profiles ||
        policy.profiles > 100000 || !policy.buckets_per_profile ||
        policy.buckets_per_profile > 4096 || !policy.instructions ||
        policy.instructions > 1000000 || !policy.output_bytes ||
        policy.output_bytes > 128 * 1024 * 1024)
        throw std::invalid_argument("invalid Prolific speculation policy");
    detail::validate_report(Report{snapshot, {}, "not_requested", {}});
    std::map<Key, Profile> profiles;
    for (const auto &event : snapshot.events) {
        if (!event.name.starts_with("prolific.profile/"))
            continue;
        auto o = observation(event);
        const auto outcome = numeric(o.kind) ? std::to_string(event.value) : o.outcome;
        Key key{event.domain,        o.kind,
                o.context,           o.site,
                o.dimension,         o.unit,
                event.location.file, event.location.ir_id,
                event.location.line, event.location.column};
        auto found = profiles.find(key);
        if (found == profiles.end()) {
            if (profiles.size() == policy.profiles)
                throw std::length_error("Prolific profile budget exhausted");
            found = profiles
                        .emplace(std::move(key), Profile{std::move(o),
                                                         event.domain,
                                                         event.location,
                                                         0,
                                                         0,
                                                         event.sequence,
                                                         0,
                                                         {}})
                        .first;
        }
        auto &p = found->second;
        ++p.count;
        p.last = event.sequence;
        auto bucket = p.buckets.find(outcome);
        if (bucket != p.buckets.end())
            ++bucket->second;
        else if (p.buckets.size() < policy.buckets_per_profile)
            p.buckets.emplace(outcome, 1);
        else
            ++p.omitted;
        if (numeric(p.observation.kind)) {
            p.min = std::min(p.min, event.value);
            p.max = std::max(p.max, event.value);
            if (!p.overflow) {
                if ((event.value > 0 && p.sum > INT64_MAX - event.value) ||
                    (event.value < 0 && p.sum < INT64_MIN - event.value))
                    p.overflow = true;
                else
                    p.sum += event.value;
            }
        }
    }
    Output out{{}, policy.output_bytes, 0, policy.instructions};
    out.append("local ir = require('lspecir')\nreturn "
               "ir.module{version=1,source_schema='moonstone.prolific/1',\n");
    out.append("created_unix_ns=" + number(snapshot.created_unix_ns) + ",complete=" +
               (snapshot.complete() ? "true" : "false") + ",rejected=" + number(snapshot.rejected) +
               ",open_spans=" + number(snapshot.open_spans) +
               ",minimum_samples=" + number(policy.minimum_samples) + ",dominance_permille=" +
               std::to_string(policy.dominance_permille) + ",instructions={\n");
    std::size_t serial = 0;
    auto warning = [&](std::string_view profile, std::string_view code, std::string_view message) {
        out.instruction("warning", "id=" + quoted("w" + std::to_string(++serial)) +
                                       ",profile=" + quoted(profile) + ",code=" + quoted(code) +
                                       ",message=" + quoted(message));
    };
    if (!snapshot.complete())
        warning("", "INCOMPLETE",
                "Collection lost observations or has unclosed spans; speculative candidates are "
                "suppressed.");
    if (profiles.empty())
        warning("", "NO-PROFILES", "No structured profile observations were supplied.");
    std::size_t profile_id = 0;
    for (const auto &[key, p] : profiles) {
        (void)key;
        const auto id = "p" + std::to_string(++profile_id);
        const auto &o = p.observation;
        std::string fields = "id=" + quoted(id) + ",category=" + quoted(name(o.kind)) +
                             ",domain=" + quoted(name(p.domain)) + ",site=" + quoted(o.site) +
                             ",context=" + quoted(o.context) + ",dimension=" + quoted(o.dimension) +
                             ",unit=" + quoted(o.unit) + ",location=" + location(p.location) +
                             ",samples=" + number(p.count) + ",omitted=" + number(p.omitted) +
                             ",first=" + number(p.first) + ",last=" + number(p.last) +
                             ",numeric=" + (numeric(o.kind) ? "true" : "false") + ",buckets={";
        std::uint64_t matches = 0;
        std::string expected;
        for (const auto &[value, count] : p.buckets) {
            fields += "{value=" + quoted(value) + ",count=" + number(count) + "},";
            if (count > matches) {
                matches = count;
                expected = value;
            }
        }
        fields += "}";
        if (numeric(o.kind))
            fields += ",minimum=" + integer(p.min) + ",maximum=" + integer(p.max) +
                      ",sum=" + quoted(p.overflow ? "" : std::to_string(p.sum)) +
                      ",sum_overflow=" + (p.overflow ? "true" : "false");
        out.instruction("profile", fields);
        if (!snapshot.complete())
            continue;
        if (p.omitted) {
            warning(id, "TRUNCATED",
                    "Histogram cardinality exceeded the budget; no candidate emitted.");
            continue;
        }
        if (p.count < policy.minimum_samples) {
            warning(id, "INSUFFICIENT",
                    "Too few observations for the configured minimum sample count.");
            continue;
        }
        const bool dominant = matches * 1000 >= p.count * policy.dominance_permille;
        const bool range = numeric(o.kind) && (!dominant || o.kind == ProfileKind::Cost);
        if (!dominant && !range) {
            warning(id, "POLYMORPHIC",
                    "No outcome met the configured dominance threshold; retain general behavior.");
            continue;
        }
        if (range) {
            expected.clear();
            matches = p.count;
        }
        const auto r = rule(o.kind, range);
        const auto guard = "g" + std::to_string(++serial);
        out.instruction("guard", "id=" + quoted(guard) + ",profile=" + quoted(id) + ",predicate=" +
                                     quoted(r.predicate) + ",expected=" + quoted(expected) +
                                     ",minimum=" + quoted(range ? std::to_string(p.min) : "") +
                                     ",maximum=" + quoted(range ? std::to_string(p.max) : "") +
                                     ",phase=" + quoted(r.phase) + ",on_miss=" + quoted(r.miss) +
                                     ",matches=" + number(matches) + ",samples=" + number(p.count) +
                                     ",evidence='observed',enforced=false");
        out.instruction("advice", "id=" + quoted("a" + std::to_string(++serial)) +
                                      ",profile=" + quoted(id) + ",guard=" + quoted(guard) +
                                      ",code=" + quoted("SPECULATE-" + std::string(name(o.kind))) +
                                      ",message=" + quoted(r.advice) + ",confidence='heuristic'");
        if (o.kind == ProfileKind::Interoperation || o.kind == ProfileKind::System ||
            o.kind == ProfileKind::Lowering)
            warning(id, "SEMANTICS-REQUIRED",
                    "Observed identities do not establish ABI compatibility, effect freedom or "
                    "lowering legality; validate these separately.");
        if (p.overflow)
            warning(
                id, "SUM-OVERFLOW",
                "The numeric sum overflowed; the range and individual observations remain exact.");
    }
    out.append("}}\n");
    return std::move(out.text);
}
} // namespace moonstone::prolific
