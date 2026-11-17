// Source-level linker extension SDK. No Lua headers leak into client code.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace qld {
inline constexpr unsigned ScriptSDKVersion = 1;
class ScriptSDK {
public:
    using Arguments = std::vector<std::string>;
    using Directive = std::function<std::string(const Arguments &)>;
    // Invoke source extensions on a separate line: @name arg, "quoted arg".
    // Returned source is recursively expanded, then parsed by the linker.
    // Duplicate/invalid names and malformed expansions throw std::runtime_error.
    void registerDirective(std::string name, Directive);
    void defineMacro(std::string name, Arguments parameters, std::string body);
    std::string preprocess(const std::string &source,
                           const std::string &name = "<script>") const;
    // Lua explicitly selected here, by a .lua filename, or a --!lua first line.
    // qld.emit/entry/input/search_dir/section/define/memory/assertion create
    // ordinary linker commands; Lua functions provide script macro facilities.
    std::string lua(const std::string &source,
                    const std::string &name = "<lua-script>") const;
private:
    std::map<std::string, Directive> directives_;
};
}
