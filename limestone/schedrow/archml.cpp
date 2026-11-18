#include "archml.hpp"

namespace limestone::schedrow {
Result<ArchMLSchedulingInput> bind_archml_schedule(const Region& input,const metacode::ArchMLProfile& profile,
                                                  std::string_view microarchitecture,const MachineModel& policy) {
  auto schedule=profile.scheduling(microarchitecture);
  if(!schedule)return Result<ArchMLSchedulingInput>::err(schedule.error());
  return detail::bind_archml_schedule(input,schedule.value(),profile.name()+"/"+std::string(microarchitecture),policy);
}

Result<ArchMLSchedulingInput> detail::bind_archml_schedule(const Region& input,const metacode::ArchMLSchedule& model,
                                                          std::string_view name,const MachineModel& policy) {
  ArchMLSchedulingInput out{input,policy};
  out.machine.issue_width=model.issue_width;
  out.machine.resource_capacity.clear();
  out.machine.resource_capacity.insert(model.resource_capacity.begin(),model.resource_capacity.end());
  for(auto& instruction:out.region.instructions) {
    auto timing=model.instructions.find(instruction.opcode);
    if(timing==model.instructions.end())
      return Result<ArchMLSchedulingInput>::err({Error::Code::Unsupported,
        std::string(name)+": no schedule for "+instruction.opcode+
        " (instruction "+std::to_string(instruction.id)+
        (instruction.origin.empty()?"":" at "+instruction.origin)+")"});
    const auto& cost=timing->second;
    instruction.latency=cost.latency;
    instruction.latency_range.reset();
    instruction.throughput=cost.throughput_cycles;
    instruction.barrier|=cost.barrier;
    // Uses here are issue-slot reservations, not functional-unit occupancy;
    // throughput is separate and is not a reservation duration.
    instruction.resources.clear();
    for(const auto& resource:cost.resources)
      instruction.resources.push_back({resource,1,1,0,{}});
  }
  auto valid=validate_region(out.region);
  if(!valid)return Result<ArchMLSchedulingInput>::err(valid.error());
  return Result<ArchMLSchedulingInput>::ok(std::move(out));
}
}
