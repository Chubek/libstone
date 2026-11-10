#pragma once
#include <cstddef>
#include <string>
#include <vector>
#if __has_include(<EquinoxNG.hpp>)
#include <EquinoxNG.hpp>
#define WETSTONE_HAS_EQUINOX_NG 1
#else
#define WETSTONE_HAS_EQUINOX_NG 0
#endif
namespace wetstone::integration {
inline constexpr bool equinox_ng_available=WETSTONE_HAS_EQUINOX_NG;
#if WETSTONE_HAS_EQUINOX_NG
struct SaturationResult {
    equinoxng::Term optimized;
    equinoxng::RunnerStats stats;
    std::size_t cost=0;
};
inline SaturationResult saturate(const equinoxng::Term& term,
                 const std::vector<equinoxng::RewriteRule>& rules,
                 equinoxng::RunnerConfig config={}) {
    equinoxng::EGraph egraph;
    auto root=egraph.add(term);
    auto stats=egraph.run(rules,config);
    auto answer=egraph.extract(root);
    return {answer.term,stats,answer.cost};
}
#endif
}
