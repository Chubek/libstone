#pragma once
#include "Diagnostic.hpp"
#include <sstream>
namespace wetstone::sarif {
inline std::string escape(const std::string& s) {
    std::string x;for(unsigned char c:s) {switch(c) {
        case '"':x+="\\\"";break;case '\\':x+="\\\\";break;
        case '\n':x+="\\n";break;case '\r':x+="\\r";break;case '\t':x+="\\t";break;
        default:if(c<32){static const char* hex="0123456789abcdef";x+="\\u00";x+=hex[c>>4];x+=hex[c&15];}else x+=static_cast<char>(c);
    }}return x;
}
inline std::string serialize(const Diagnostics& ds,std::string tool="Wetstone") {
    std::ostringstream o;
    o<<"{\"version\":\"2.1.0\",\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\",\"runs\":[{\"tool\":{\"driver\":{\"name\":\""<<escape(tool)<<"\"}},\"results\":[";
    bool first=true;
    for(const auto& d:ds){if(!first)o<<',';first=false;
        o<<"{\"ruleId\":\""<<escape(d.rule)<<"\",\"level\":\""<<severity_name(d.severity)
         <<"\",\"message\":{\"text\":\""<<escape(d.message)<<"\"}";
        if(!d.file.empty())o<<",\"locations\":[{\"physicalLocation\":{\"artifactLocation\":{\"uri\":\""<<escape(d.file)
            <<"\"},\"region\":{\"startLine\":"<<(d.line?d.line:1)<<"}}}]";
        o<<'}';
    }o<<"]}]}";return o.str();
}
}
