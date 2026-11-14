#include <exoml/CDecl.hpp>
#include <exoml/StaticFFI.hpp>
#ifdef TEST_EXTRACT
#include <exoml/Extract.hpp>
#endif
int main() {
    moonstone::exoml::cdecl::TranslationUnit empty;
    if (moonstone::exoml::cdecl::describe_translation_unit(empty).empty())
        return 1;
#ifdef TEST_EXTRACT
    moonstone::exoml::cdecl::ExtractOptions options;
    options.header = EXTRACT_HEADER;
    moonstone::exoml::cdecl::CacheReport report;
    auto discovered = moonstone::exoml::cdecl::extract_c11(options, report);
    if (!discovered.complete || discovered.declarations.size() != 1 ||
        discovered.declarations[0].name != "installed_add" ||
        report.status != moonstone::exoml::cdecl::CacheStatus::Disabled)
        return 1;
#ifdef EXTRACT_CACHE_DIRECTORY
    options.cache_directory = EXTRACT_CACHE_DIRECTORY;
    auto cached = moonstone::exoml::cdecl::extract_c11(options, report);
    if (!cached.complete)
        return 1;
    auto warm = moonstone::exoml::cdecl::extract_c11(options, report);
    if (!warm.complete || report.status != moonstone::exoml::cdecl::CacheStatus::Hit ||
        moonstone::exoml::cdecl::describe_translation_unit(cached) !=
            moonstone::exoml::cdecl::describe_translation_unit(warm))
        return 1;
#endif
#endif
    moonstone::exoml::Interpreter vm;
    moonstone::exoml::bind_native(vm, "Math:Add", [](int a, int b) { return a + b; });
    vm.enable_hatch(true);
    vm.allow_service("Math:Add");
    auto errors = vm.execute(moonstone::exoml::Parser("X ~hatch! Math:Add(19,23);").parse());
    return !errors.empty() || std::get<std::int64_t>(vm.symbols().at("X")) != 42;
}
