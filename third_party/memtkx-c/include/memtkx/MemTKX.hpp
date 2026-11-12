#pragma once
/**
 * @file memtkx/MemTKX.hpp
 * @brief Umbrella header of the header-only C++ wrapper over the MemTKX C
 *        library. Link `MemTKX::MemTKXCpp` (or `MemTKX::MemTKX`, C++17).
 *
 * The `MemTKX` namespace keeps the original C++ API. Each wrapper owns or
 * views a C object and exposes it for interoperability (`c_space()`,
 * `c_plan()`, `c_table()`, `c_node()`, ...); `Slot` and `ReadSlot` are the
 * C structs themselves. Application spaces and plans written against
 * `SpaceBase` / `PlanBase` are usable from C through those accessors.
 */

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/core/units.hpp"

#include "memtkx/space/bump_pointer.hpp"
#include "memtkx/space/free_list.hpp"
#include "memtkx/space/immix_space.hpp"
#include "memtkx/space/los_space.hpp"
#include "memtkx/space/space.hpp"

#include "memtkx/barrier/barrier.hpp"
#include "memtkx/barrier/card_table.hpp"
#include "memtkx/barrier/satb.hpp"

#include "memtkx/scheduler/coordinator.hpp"
#include "memtkx/scheduler/task_pipeline.hpp"
#include "memtkx/scheduler/work_packet.hpp"

#include "memtkx/plan/generational.hpp"
#include "memtkx/plan/immix.hpp"
#include "memtkx/plan/marksweep.hpp"
#include "memtkx/plan/plan.hpp"
#include "memtkx/plan/semispace.hpp"
