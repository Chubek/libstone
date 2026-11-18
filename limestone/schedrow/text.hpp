#pragma once
#include "schedrow.hpp"
#include "metacode/metacode.hpp"
#include "metacode/archml.hpp"
#include <map>

namespace limestone::schedrow {
struct TextRegion {
  Region region;
  std::map<std::string,uint32_t> instruction_names,value_names,physical_names;
  metacode::Value::Object metadata;
  std::map<InstrId,metacode::Value::Object> instruction_metadata;
};
struct SchedulingDocument {
  std::string machine_name;
  MachineModel machine;
  metacode::Value::Object machine_metadata;
  std::vector<TextRegion> regions;
};
Result<SchedulingDocument> load_schedrow(std::string_view,std::string_view file="<schedrow>");
// Uses ArchML for default latency and replaces the document's issue/resource
// model, while retaining the document's register aliases and search policy.
Result<SchedulingDocument> load_schedrow(std::string_view,std::string_view file,
                                        const metacode::ArchMLProfile&,std::string_view microarchitecture);
Result<std::string> print_schedrow(const SchedulingDocument&);
}
