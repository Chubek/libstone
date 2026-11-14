#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace moonstone::exotract::detail {
struct Json {
    enum class Type { Null, Boolean, Number, String, Object, Array } type = Type::Null;
    std::string value;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json &at(const std::string &key) const {
        if (type != Type::Object) throw std::invalid_argument("expected JSON object");
        auto i = object.find(key);
        if (i == object.end()) throw std::invalid_argument("missing JSON key: " + key);
        return i->second;
    }
    std::string str() const {
        if (type != Type::String) throw std::invalid_argument("expected JSON string");
        return value;
    }
    bool boolean() const {
        if (type != Type::Boolean) throw std::invalid_argument("expected JSON boolean");
        return value == "true";
    }
};
class JsonParser {
    std::string_view s;
    std::size_t i = 0, nodes = 0;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
    void expect(char c) { ws(); if (i == s.size() || s[i++] != c) throw std::invalid_argument("malformed JSON"); }
    static int hex(char c) {
        if(c>='0'&&c<='9') return c-'0';
        if(c>='a'&&c<='f') return c-'a'+10;
        if(c>='A'&&c<='F') return c-'A'+10;
        throw std::invalid_argument("invalid JSON unicode hex");
    }
    void cp(std::string &out, std::uint32_t c) {
        if (c <= 0x7f) out += static_cast<char>(c);
        else if (c <= 0x7ff) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else if (c <= 0xffff) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    std::string string() {
        expect('"'); std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 32) throw std::invalid_argument("JSON control character");
            if (c != '\\') { out += c; continue; }
            if (i == s.size()) break;
            c = s[i++];
            switch(c) {
                case '"': out += '"'; break; case '\\': out += '\\'; break;
                case '/': out += '/'; break; case 'b': out += '\b'; break;
                case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) throw std::invalid_argument("short JSON unicode");
                    std::uint32_t v = 0;
                    for (int k = 0; k < 4; ++k) v = (v << 4) | hex(s[i++]);
                    if (v >= 0xd800 && v <= 0xdbff) {
                        if (i + 6 > s.size() || s[i++] != '\\' || s[i++] != 'u')
                            throw std::invalid_argument("missing JSON surrogate pair");
                        std::uint32_t low = 0;
                        for (int k = 0; k < 4; ++k) low = (low << 4) | hex(s[i++]);
                        if (low < 0xdc00 || low > 0xdfff) throw std::invalid_argument("invalid JSON low surrogate");
                        v = 0x10000 + ((v - 0xd800) << 10) + (low - 0xdc00);
                    } else if (v >= 0xdc00 && v <= 0xdfff)
                        throw std::invalid_argument("unpaired JSON surrogate");
                    cp(out,v); break;
                }
                default: throw std::invalid_argument("invalid JSON escape");
            }
        }
        throw std::invalid_argument("unterminated JSON string");
    }
    Json parse_one(unsigned depth) {
        if (depth > 128 || ++nodes > 1000000) throw std::length_error("JSON complexity budget exceeded");
        ws(); if (i == s.size()) throw std::invalid_argument("unexpected EOF JSON");
        Json j;
        if (s[i] == '"') { j.type = Json::Type::String; j.value = string(); }
        else if (s[i] == '{') {
            ++i; j.type = Json::Type::Object;
            ws(); if(i<s.size() && s[i]=='}'){++i;return j;}
            for (;;) {
                ws(); auto key = string(); expect(':');
                auto [iter, inserted] = j.object.emplace(key,parse_one(depth+1));
                if (!inserted) throw std::invalid_argument("duplicate JSON key: " + key);
                ws(); if(i<s.size()&&s[i]=='}'){++i;break;} expect(',');
            }
        } else if (s[i] == '[') {
            ++i; j.type = Json::Type::Array;
            ws(); if(i<s.size()&&s[i]==']'){++i;return j;}
            for (;;) {
                j.array.emplace_back(parse_one(depth+1));
                ws(); if(i<s.size()&&s[i]==']'){++i;break;} expect(',');
            }
        } else {
            auto starts = [&](std::string_view t){return s.substr(i,t.size())==t;};
            if(starts("true")){j.type=Json::Type::Boolean;j.value="true";i+=4;}
            else if(starts("false")){j.type=Json::Type::Boolean;j.value="false";i+=5;}
            else if(starts("null")){i+=4;}
            else {
                const auto start = i;
                if(s[i]=='-') ++i;
                if(i>=s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
                    throw std::invalid_argument("invalid JSON number");
                if(s[i]=='0') ++i;
                else while(i<s.size()&&std::isdigit(static_cast<unsigned char>(s[i])))++i;
                if(i<s.size()&&s[i]=='.') {
                    ++i; if(i==s.size()||!std::isdigit(static_cast<unsigned char>(s[i])))throw std::invalid_argument("invalid JSON fraction");
                    while(i<s.size()&&std::isdigit(static_cast<unsigned char>(s[i])))++i;
                }
                if(i<s.size()&&(s[i]=='e'||s[i]=='E')){
                    ++i;if(i<s.size()&&(s[i]=='-'||s[i]=='+'))++i;
                    if(i==s.size()||!std::isdigit(static_cast<unsigned char>(s[i])))throw std::invalid_argument("invalid JSON exponent");
                    while(i<s.size()&&std::isdigit(static_cast<unsigned char>(s[i])))++i;
                }
                j.type=Json::Type::Number;j.value=std::string(s.substr(start,i-start));
            }
        }
        return j;
    }
public:
    explicit JsonParser(std::string_view text):s(text) { if(s.size()>128*1024*1024) throw std::length_error("JSON byte budget"); }
    Json parse() { auto j=parse_one(0); ws(); if(i!=s.size()) throw std::invalid_argument("trailing JSON text");return j; }
};
inline std::vector<Symbol> symbols_from_cdecl(const Json &root) {
    const auto &decls=root.at("declarations");
    if(decls.type!=Json::Type::Array)throw std::invalid_argument("CDecl declarations must be an array");
    std::vector<Symbol> symbols;
    for(const auto &decl: decls.array){
        const auto kind=decl.at("kind").str();
        if(kind!="function" && kind!="typedef" && kind!="record" && kind!="enum" && kind!="variable")continue;
        Symbol sym;
        sym.name=decl.at("name").str();
        if(sym.name.empty()) continue;
        sym.kind=kind;
        sym.type=decl.at("type").str();
        auto it=decl.object.find("linkage");if(it!=decl.object.end())sym.linkage=it->second.str();
        auto jt=decl.object.find("unsupported_reason");
        sym.supported=(jt==decl.object.end() || jt->second.str().empty());
        symbols.push_back(std::move(sym));
    }
    return symbols;
}
}
