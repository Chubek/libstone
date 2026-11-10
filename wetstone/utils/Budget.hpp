#pragma once
#include <cstddef>
namespace wetstone {
struct Budget {std::size_t limit=100000,used=0;bool exhausted()const noexcept{return used>=limit;}bool consume()noexcept{if(exhausted())return false;++used;return true;}};
}
