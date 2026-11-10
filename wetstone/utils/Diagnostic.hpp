#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace wetstone {
enum class Severity { Note, Warning, Error };
struct Diagnostic {
    std::string rule,message,file,function;
    std::uint32_t line=0,block=0;
    Severity severity=Severity::Warning;
};
using Diagnostics=std::vector<Diagnostic>;
inline const char* severity_name(Severity s) {
    switch(s){case Severity::Note:return "note";case Severity::Warning:return "warning";case Severity::Error:return "error";}return "warning";
}
}
