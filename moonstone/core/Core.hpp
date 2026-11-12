#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonstone {
struct SourceSpan { std::string file; std::uint32_t line=1,column=1,end_line=1,end_column=1; };
enum class Severity { Note, Warning, Error, Fatal };
inline const char* name(Severity s) { switch(s){case Severity::Note:return "note"; case Severity::Warning:return "warning";case Severity::Error:return "error";case Severity::Fatal:return "fatal";}return "error"; }
struct Fix { SourceSpan span; std::string replacement, description; bool validated=false; };
struct Diagnostic {
 std::string id, message; Severity severity=Severity::Error; SourceSpan primary;
 std::vector<SourceSpan> related; std::vector<std::string> notes; std::vector<Fix> fixes;
};
using Diagnostics=std::vector<Diagnostic>;
enum class Status { Proven, Refuted, Unknown, BudgetExhausted, InvalidInput };
struct ProofResult { Status status=Status::Unknown; std::string explanation; std::vector<std::string> assumptions; std::size_t work=0; bool is_proven()const{return status==Status::Proven;} };
inline std::string json_escape(std::string_view s) { std::string out; for(unsigned char c:s){ switch(c){case '\\':out+="\\\\";break;case '"':out+="\\\"";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default: if(c<32){const char* hex="0123456789abcdef";out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}}return out; }
inline std::string format(const Diagnostic& d){std::ostringstream o;o<<d.primary.file<<':'<<d.primary.line<<':'<<d.primary.column<<": "<<name(d.severity)<<'['<<d.id<<"]: "<<d.message;for(auto& n:d.notes)o<<"\n  note: "<<n;return o.str();}
// Source edits are applied only when byte offsets have been independently checked by the host.
struct SourceEdit { std::size_t begin=0,end=0; std::string replacement; };
inline std::optional<std::string> apply_edits(const std::string& source,std::vector<SourceEdit> edits){
 std::sort(edits.begin(),edits.end(),[](auto& a,auto& b){return a.begin<b.begin;});
 std::size_t cursor=0;std::string result;
 for(auto& e:edits){if(e.begin<cursor||e.end<e.begin||e.end>source.size())return std::nullopt;result.append(source,cursor,e.begin-cursor);result+=e.replacement;cursor=e.end;}
 result.append(source,cursor,std::string::npos);return result;
}
}
