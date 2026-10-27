#pragma once
#include "Common.hpp"
namespace firestone::regalloc {
struct PressurePoint {
    Position position = 0;
    std::size_t live = 0;
    std::map<std::string, std::size_t> classes;
};
struct PressureProfile {
    std::vector<PressurePoint> points;
    std::size_t peak = 0;
    std::map<std::string, std::size_t> peak_by_class;
};
struct RegisterPressure {
    PressureProfile estimate(const Problem& p) const {
        struct Events { std::vector<ValueId> begin, end; };
        std::map<Position, Events> events;
        for (ValueId v = 0; v < p.size(); ++v) {
            if (p.value(v).ranges.empty()) throw std::invalid_argument("pressure estimation requires live ranges");
            for (auto r : p.value(v).ranges) { events[r.begin].begin.push_back(v); events[r.end].end.push_back(v); }
        }
        PressureProfile out; PressurePoint point;
        for (const auto& event : events) {
            point.position = event.first;
            for (auto v : event.second.end) { --point.live; --point.classes[p.value(v).reg_class]; }
            for (auto v : event.second.begin) { ++point.live; ++point.classes[p.value(v).reg_class]; }
            out.peak = std::max(out.peak, point.live);
            for (const auto& c : point.classes) out.peak_by_class[c.first] = std::max(out.peak_by_class[c.first], c.second);
            out.points.push_back(point);
        }
        return out;
    }
};
} // namespace firestone::regalloc
