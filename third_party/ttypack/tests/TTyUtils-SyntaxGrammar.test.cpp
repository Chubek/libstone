#include <TTyUtils/TTyUtils-SyntaxGrammar.hpp>
#include <cassert>
#include <fstream>

int main() {
    using namespace ttyutils::syntax;
    const auto directory = std::filesystem::temp_directory_path() / "ttypack-SyntaxGrammar";
    std::filesystem::create_directories(directory / "ignored");
    const auto path = directory / "grammar.tmLanguage.json";
    std::ofstream{path} << R"json({
       "scopeName":"source.cpp","name":"C\"\u002b+","fileTypes":["cpp","Makefile"],
       "keywords":["int"], "patterns":[{"include":"#main"}],
       "repository":{"main":{"patterns":[{"include":"#main"},{"match":"\\bint\\b","name":"keyword"}]}}
     })json";
    const auto other = directory / "native.json";
    std::ofstream{other} << R"json({"id":"python","globs":["*.py"],"firstLineMatch":"^#!.*python","keywords":["def"]})json";
    auto grammar = load_tm(path);
    assert(grammar);
    assert(grammar.value().matches_filename("src/main.cpp"));
    assert(grammar.value().matches_filename("src/Makefile"));
    assert(grammar.value().name == "C\"++");
    assert(grammar.value().rules.size() == 1 && grammar.value().rules[0].match == "\\bint\\b");
    Registry registry;
    registry.load_dir(directory);
    const auto cpp = registry.by_id("cpp");
    const auto python = registry.by_id("python");
    assert(cpp && python);
    assert(detect(registry, "unknown", "#!/usr/bin/env -S python3 -u") == python);
    assert(detect(registry, "unknown", "#!/bin/sh") == nullptr);
    assert(detect(registry, "main.cpp", "# -*- mode: python; flags: none -*-") == python);
    registry.add(Grammar{});
    assert(cpp == registry.by_id("source.cpp"));
    std::ofstream{path, std::ios::trunc} << R"({"scopeName":"source.fail","name":"\uD800"})";
    assert(!load_tm(path));
    std::filesystem::remove(path);
    std::filesystem::remove(other);
    std::filesystem::remove(directory / "ignored");
    std::filesystem::remove(directory);
    return 0;
}
