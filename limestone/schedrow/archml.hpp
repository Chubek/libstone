#pragma once
#include "schedrow.hpp"
#include "../metacode/archml.hpp"

namespace limestone::schedrow {
struct ArchMLSchedulingInput {
  Region region;
  MachineModel machine;
};
// Bind the region to one explicit microarchitecture. All opcodes must name
// scheduled ArchML forms. The profile supplies default latency, throughput,
// issue width and resources; other region constraints and machine policy
// (including register aliases) are preserved.
Result<ArchMLSchedulingInput> bind_archml_schedule(const Region&,const metacode::ArchMLProfile&,
                                                   std::string_view microarchitecture,const MachineModel& policy={});

namespace detail {
Result<ArchMLSchedulingInput> bind_archml_schedule(const Region&,const metacode::ArchMLSchedule&,
                                                   std::string_view model_name,const MachineModel& policy);
}
}
