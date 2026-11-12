/**
 * @file memtkx/memtkx.h
 * @brief Umbrella header for the MemTKX C library
 *        (the C counterpart of <memtkx/MemTKX.hpp>).
 *
 * MemTKX is a C99 memory-management toolkit for building allocators,
 * garbage-collection experiments and runtime integrations. Public names
 * use the `mtkx_` prefix (types, functions) and `MTKX_` (macros).
 */
#ifndef MEMTKX_MEMTKX_H
#define MEMTKX_MEMTKX_H

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/core/units.h"
#include "memtkx/core/vec.h"
#include "memtkx/core/version.h"

#include "memtkx/space/bump_pointer.h"
#include "memtkx/space/free_list.h"
#include "memtkx/space/immix_space.h"
#include "memtkx/space/los_space.h"
#include "memtkx/space/space.h"

#include "memtkx/barrier/barrier.h"
#include "memtkx/barrier/card_table.h"
#include "memtkx/barrier/satb.h"

#include "memtkx/scheduler/coordinator.h"
#include "memtkx/scheduler/task_pipeline.h"
#include "memtkx/scheduler/work_packet.h"

#include "memtkx/plan/generational.h"
#include "memtkx/plan/immix.h"
#include "memtkx/plan/marksweep.h"
#include "memtkx/plan/plan.h"
#include "memtkx/plan/semispace.h"

#include "memtkx/dsl.h"

#endif /* MEMTKX_MEMTKX_H */
